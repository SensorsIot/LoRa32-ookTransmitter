/** @file
    Euromot awning remote decoder for rtl_433_ESP (FR-11).

    The awning controller's own remote. OOK-PWM, an 18-bit fixed codeword:
    a 12-bit device id (0x7F4) followed by a 6-bit button code. Derived from
    the controller's authoritative transmit codes (src/config.cpp, bits 19..2):
      up=0x15  down=0x17  auto=0x20  manual=0x21   (all under id 0x7F4)
    Pulse timing measured from the physical remote on the workbench RTL-SDR
    (short mark 340 us, long mark 2068 us, inter-word reset 13936 us); a wide
    tolerance also covers the controller's slightly different transmit timing.

    Registered at runtime via register_protocol() from rx433.cpp, so this lives
    in the project (no rtl_433_ESP fork).
*/

#include "decoder.h"

static int euromot_awning_decode(r_device *decoder, bitbuffer_t *bitbuffer)
{
    // The remote's ~15.6 ms inter-word gap exceeds reset_limit, so each word is
    // captured as its own single-row signal. Decode from any valid row rather
    // than requiring a repeat within one bitbuffer.
    for (int row = 0; row < bitbuffer->num_rows; row++) {
    int nbits = bitbuffer->bits_per_row[row];
    if (nbits < 17 || nbits > 20)
        continue;

    uint8_t *b = bitbuffer->bb[row];
    // 18 bits, MSB-first: [id:12][button:6].
    uint32_t code = (((uint32_t)b[0] << 10) | ((uint32_t)b[1] << 2) | (b[2] >> 6)) & 0x3FFFF;
    uint32_t id   = code >> 6;
    uint32_t btn  = code & 0x3F;

    if (id != 0x7F4)
        continue;   // not a Euromot frame

    const char *button;
    switch (btn) {
        case 0x15: button = "up";      break;
        case 0x17: button = "down";    break;
        case 0x20: button = "auto";    break;
        case 0x21: button = "manual";  break;
        default:   button = "unknown"; break;
    }

    char code_str[8];
    snprintf(code_str, sizeof(code_str), "%05X", (unsigned)code);

    /* clang-format off */
    data_t *data = data_make(
            "model",  "",       DATA_STRING, "Euromot-Awning",
            "id",     "",       DATA_INT,    (int)id,
            "button", "Button", DATA_STRING, button,
            "code",   "Code",   DATA_STRING, code_str,
            NULL);
    /* clang-format on */
    decoder_output_data(decoder, data);
    return 1;
    }
    return DECODE_ABORT_EARLY;
}

static char const *const euromot_awning_fields[] = {
        "model",
        "id",
        "button",
        "code",
        NULL,
};

r_device euromot_awning = {
        .name        = "Euromot-Awning",
        .modulation  = OOK_PULSE_PWM,
        .short_width = 340,
        .long_width  = 2068,
        .gap_limit   = 2000,
        .reset_limit = 13936,
        .tolerance   = 691,
        .decode_fn   = &euromot_awning_decode,
        .disabled    = 0,
        .fields      = euromot_awning_fields,
};
