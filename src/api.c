#include "api.h"
#include "common.h"
#include "aux.h"
#include "iota_io.h"
#include "ui/ui.h"

// iota-related stuff
#include "iota/conversion.h"
#include "iota/addresses.h"
#include "iota/bundle.h"
#include "iota/seed.h"
#include "iota/signing.h"

// TODO: we don't want to include main.h
bool flash_is_init();

#define CHECK_STATE(state, INS)                                                \
    ((((state)&INS##_REQUIRED_STATE) != INS##_REQUIRED_STATE) ||               \
     ((state)&INS##_FORBIDDEN_STATE))

#define GET_INPUT(input_data, len, INS)                                        \
    ({                                                                         \
        if (!flash_is_init())                                                  \
            THROW(SW_APP_NOT_INITIALIZED);                                     \
        if (CHECK_STATE(api.state_flags, INS))                                 \
            THROW(SW_COMMAND_INVALID_STATE);                                   \
        if (len < sizeof(INS##_INPUT))                                         \
            THROW(SW_WRONG_LENGTH);                                            \
        (INS##_INPUT *)(input_data);                                           \
    })

typedef struct API_CTX {

    unsigned char seed_bytes[48];
    uint8_t security; // use hard-coded security for now
    
    unsigned int active_seed;

    BUNDLE_CTX bundle_ctx;
    SIGNING_CTX signing_ctx;

    unsigned int state_flags;
} API_CTX;

// global variable storing all data needed across multiple api callss
API_CTX api;

void api_initialize()
{
    os_memset(&api, 0, sizeof(api));
}

unsigned int api_set_seed(const unsigned char *input_data, unsigned int len)
{
    const SET_SEED_INPUT *input = GET_INPUT(input_data, len, SET_SEED);

    // setting the seed resets everything
    api_initialize();

    unsigned int path[BIP44_PATH_LEN];
    for (unsigned int i = 0; i < BIP44_PATH_LEN; i++) {
        if (!ASSIGN(path[i], input->bip44_path[i])) {
            // path overflow
            THROW(SW_COMMAND_INVALID_DATA);
        }
    }
    
    // set our currently active seed
    api.active_seed = path[BIP44_PATH_LEN-1];

    if (!ASSIGN(api.security, input->security) ||
        !IN_RANGE(api.security, MIN_SECURITY_LEVEL, MAX_SECURITY_LEVEL)) {
        // invalid security
        THROW(SW_COMMAND_INVALID_DATA);
    }

    derive_seed_bip32(path, BIP44_PATH_LEN, api.seed_bytes);

    api.state_flags |= SEED_SET;

    io_send(NULL, 0, SW_OK);
    return 0;
}

unsigned int api_pubkey(const unsigned char *input_data, unsigned int len)
{
    const PUBKEY_INPUT *input = GET_INPUT(input_data, len, PUBKEY);

    ui_display_calc();

    uint32_t address_idx;
    if (!ASSIGN(address_idx, input->address_idx)) {
        // address index overflow
        THROW(SW_COMMAND_INVALID_DATA);
    }

    unsigned char addr_bytes[48];
    get_public_addr(api.seed_bytes, address_idx, api.security, addr_bytes);

    PUBKEY_OUTPUT output;
    bytes_to_chars(addr_bytes, output.address, 48);

    ui_restore();

    io_send(&output, sizeof(output), SW_OK);
    return 0;
}

static void validate_tx_indices(const TX_INPUT *input)
{
    if (input->last_index != api.bundle_ctx.last_index) {
        THROW(SW_TX_INVALID_INDEX);
    }
    if (input->current_index != api.bundle_ctx.current_index) {
        THROW(SW_TX_INVALID_INDEX);
    }
}

static bool has_reference_transaction(uint32_t current_index)
{
    for (unsigned int i = 1; i < api.security; i++) {
        if (current_index < i || api.bundle_ctx.values[current_index - i] > 0) {
            return false;
        }
        if (api.bundle_ctx.values[current_index - i] < 0) {
            return true;
        }
    }

    return false;
}

static void validate_tx_order(const TX_INPUT *input)
{
    const uint32_t current_index = api.bundle_ctx.current_index;

    // the receiving addresses are only allowed first or last
    if (input->value > 0 && current_index > 0 &&
        current_index < api.bundle_ctx.last_index) {
        THROW(SW_TX_INVALID_ORDER);
    }

    // a meta transaction must have a valid reference input transaction
    if (input->value == 0 && current_index > 0 &&
        current_index < api.bundle_ctx.last_index) {
        // this must be a meta transaction
        if (!has_reference_transaction(current_index)) {
            THROW(SW_TX_INVALID_META);
        }
    }

    // the output address must come first and have positive value
    if (input->value <= 0 && current_index == 0) {
        THROW(SW_TX_INVALID_OUTPUT);
    }
}

static void get_padded_valid_tag(const char *input_tag, char *padded_tag)
{
    rpad_chars(padded_tag, input_tag, 27);
    if (!validate_chars(padded_tag, 27)) {
        // invalid tag
        THROW(SW_COMMAND_INVALID_DATA);
    }
}

unsigned int api_tx(const unsigned char *input_data, unsigned int len)
{
    const TX_INPUT *input = GET_INPUT(input_data, len, TX);

    // TODO handle not receiving complete tx
    ui_display_recv();

    if ((api.state_flags & BUNDLE_INITIALIZED) == 0) {
        uint32_t last_index;
        if (!ASSIGN(last_index, input->last_index)) {
            // last index overflow
            THROW(SW_COMMAND_INVALID_DATA);
        }
        bundle_initialize(&api.bundle_ctx, last_index);
        api.state_flags |= BUNDLE_INITIALIZED;
    }

    validate_tx_indices(input);
    validate_tx_order(input);

    if (!validate_chars(input->address, 81)) {
        // invalid address
        THROW(SW_COMMAND_INVALID_DATA);
    }

    if (input->value < 0 ||
        api.bundle_ctx.current_index == api.bundle_ctx.last_index) {
        uint32_t address_idx;
        if (!ASSIGN(address_idx, input->address_idx)) {
            // index overflow
            THROW(SW_COMMAND_INVALID_DATA);
        }
        bundle_set_internal_address(&api.bundle_ctx, input->address,
                                    address_idx);
    }
    else {
        // ignore index completely
        bundle_set_external_address(&api.bundle_ctx, input->address);
    }

    if (!IN_RANGE(input->value, -MAX_IOTA_VALUE, MAX_IOTA_VALUE)) {
        // value out of bounds
        THROW(SW_COMMAND_INVALID_DATA);
    }

    char padded_tag[27];
    get_padded_valid_tag(input->tag, padded_tag);

    uint32_t timestamp;
    if (!ASSIGN(timestamp, input->timestamp)) {
        // timestamp overflow
        THROW(SW_COMMAND_INVALID_DATA);
    }
    bundle_add_tx(&api.bundle_ctx, input->value, padded_tag, timestamp);
    if (!bundle_has_open_txs(&api.bundle_ctx)) {
        // TODO: - Basic mode check for change != 0
        ui_sign_tx(&api.bundle_ctx);
        return IO_ASYNCH_REPLY;
    }

    TX_OUTPUT output = {0};
    output.finalized = false;

    io_send(&output, sizeof(output), SW_OK);
    return 0;
}

static bool next_signatrue_fragment(SIGNING_CTX *ctx, char *signature_fragment)
{
    unsigned char fragment_bytes[SIGNATURE_FRAGMENT_SIZE * 48];
    signing_next_fragment(ctx, fragment_bytes);

    bytes_to_chars(fragment_bytes, signature_fragment,
                   SIGNATURE_FRAGMENT_SIZE * 48);

    return signing_has_next_fragment(ctx);
}

unsigned int api_sign(const unsigned char *input_data, unsigned int len)
{
    const SIGN_INPUT *input = GET_INPUT(input_data, len, SIGN);

    uint8_t tx_idx;
    if (!ASSIGN(tx_idx, input->transaction_idx) ||
        tx_idx > api.bundle_ctx.last_index) {
        // index is out of bounds
        THROW(SW_COMMAND_INVALID_DATA);
    }

    if ((api.state_flags & SIGNING_STARTED) == 0) {
        // temporary screen during signing process
        ui_display_signing();

        if (api.bundle_ctx.values[tx_idx] >= 0) {
            // no input transaction
            THROW(SW_COMMAND_INVALID_DATA);
        }

        tryte_t normalized_hash[81];
        bundle_get_normalized_hash(&api.bundle_ctx, normalized_hash);

        signing_initialize(&api.signing_ctx, tx_idx, api.seed_bytes,
                           api.bundle_ctx.indices[tx_idx], api.security,
                           normalized_hash);

        api.state_flags |= SIGNING_STARTED;
    }
    else if (tx_idx != api.signing_ctx.tx_index) {
        // transaction changed after initialization
        THROW(SW_COMMAND_INVALID_DATA);
    }

    // TODO: verify change address belongs to us in basic

    SIGN_OUTPUT output;
    output.fragments_remaining =
        next_signatrue_fragment(&api.signing_ctx, output.signature_fragment);

    io_send(&output, sizeof(output), SW_OK);

    if (!output.fragments_remaining) {

        // signing is finished
        api.state_flags &= ~SIGNING_STARTED;
        incr_seed_idx(api.active_seed);
        ui_display_welcome();
    }

    return 0;
}

unsigned int api_display_pubkey(const unsigned char *input_data,
                                unsigned int len)
{
    const PUBKEY_INPUT *input = GET_INPUT(input_data, len, PUBKEY);

    ui_display_calc();

    uint32_t address_idx;
    if (!ASSIGN(address_idx, input->address_idx)) {
        // address index overflow
        THROW(SW_COMMAND_INVALID_DATA);
    }

    unsigned char addr_bytes[48];
    get_public_addr(api.seed_bytes, address_idx, api.security, addr_bytes);

    ui_display_address(addr_bytes);

    io_send(NULL, 0, SW_OK);
    return 0;
}

/** @brief This functions gets called, when bundle is denied. */
void user_deny()
{
    // reset the bundle
    os_memset(&api.bundle_ctx, 0, sizeof(BUNDLE_CTX));
    api.state_flags &= ~BUNDLE_INITIALIZED;

    io_send(NULL, 0, SW_SECURITY_STATUS_NOT_SATISFIED);
}

static unsigned int get_change_tx_index(const BUNDLE_CTX *ctx)
{
    // there only is a proper change transaction if the value is positive
    if (ctx->values[ctx->last_index] > 0) {
        return ctx->last_index;
    }
    // return something out of bounds
    return ctx->last_index + 1;
}

/** @brief This functions gets called, when bundle is accepted. */
void user_sign()
{
    ui_display_calc();

    if (!bundle_validating_finalize(&api.bundle_ctx,
                                    get_change_tx_index(&api.bundle_ctx),
                                    api.seed_bytes, api.security)) {
        THROW(SW_SECURITY_STATUS_NOT_SATISFIED);
    }
    api.state_flags |= BUNDLE_FINALIZED;

    TX_OUTPUT output;
    output.finalized = true;
    bytes_to_chars(bundle_get_hash(&api.bundle_ctx), output.bundle_hash, 48);

    io_send(&output, sizeof(output), SW_OK);
}

void init_ledger_approve(const INIT_LEDGER_INPUT *input)
{
    // TODO test large values are properly casted
    
    // write all 5 seed indexes
    for(uint8_t i = 0; i < 5; i++) {
        write_seed_index(i, (unsigned int)input->seed_indexes[i]);
    }
    
    io_send(NULL, 0, SW_OK);
}

void init_ledger_deny()
{
    io_send(NULL, 0, SW_SECURITY_STATUS_NOT_SATISFIED);
}






/* ---------------- Basic mode functions
 */

unsigned int api_basic_pubkey(unsigned char *input_data, unsigned int len)
{
    if(!flash_is_init()) {
        THROW(SW_APP_NOT_INITIALIZED);
    }
    if (CHECK_STATE(api.state_flags, PUBKEY)) {
        THROW(SW_COMMAND_INVALID_STATE);
    }
    if (len < sizeof(PUBKEY_INPUT_BASIC)) {
        THROW(SW_WRONG_LENGTH);
    }
    if(api.active_seed > 4) {
        THROW(SW_BAD_SEED);
    }
    
    ui_display_calc();
    
    const PUBKEY_INPUT_BASIC *input = (PUBKEY_INPUT_BASIC *)(input_data);
    
    uint32_t address_idx = get_seed_idx(api.active_seed);
    
    // asking for next addr (tx change address)
    if(input->next)
        address_idx++;
    
    // TODO store address globally (as getting change addr will be last thing
    // called before submitting tx)
    unsigned char addr_bytes[48];
    get_public_addr(api.seed_bytes, address_idx, api.security, addr_bytes);
    
    PUBKEY_OUTPUT output;
    bytes_to_chars(addr_bytes, output.address, 48);
    
    ui_restore();
    
    io_send(&output, sizeof(output), SW_OK);
    return 0;
}

unsigned int api_seed_idx(unsigned char *input_data, unsigned int len)
{
    if(!flash_is_init()) {
        THROW(SW_APP_NOT_INITIALIZED);
    }
    
    const SEED_IDX_INPUT *input = (SEED_IDX_INPUT *)(input_data);
    
    if(input->account > 4)
        THROW(INVALID_PARAMETER);
    
    SEED_IDX_OUTPUT output;
    output.seed_idx = get_seed_idx(input->account);
    
    io_send(&output, sizeof(output), SW_OK);
    return 0;
}

unsigned int api_init_ledger(unsigned char *input_data, unsigned int len)
{
    if(!flash_is_init()) {
        THROW(SW_APP_NOT_INITIALIZED);
    }
    
    const INIT_LEDGER_INPUT *input = (INIT_LEDGER_INPUT *)(input_data);
    
    ui_display_init_ledger(input);
    return IO_ASYNCH_REPLY;
}
