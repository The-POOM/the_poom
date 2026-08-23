# PicoPass (HID iCLASS) reader library for POOM.
#
# Depends on the `nfcal` component, which provides ST's RFAL API
# (rfal_rf.h, rfalSetMode, rfalTransceiveBlockingTxRx, ...), and `poom_nfc`
# for bringing up the NFC core. DES/3DES are vendored (poom_des.c) because
# ESP-IDF 6.1's mbedTLS dropped MBEDTLS_DES_C.

idf_component_register(
    SRCS
        "src/rfal_picopass.c"
        "src/poom_picopass.c"
        "src/poom_des.c"
        "src/loclass/optimized_cipher.c"
        "src/loclass/optimized_cipherutils.c"
        "src/loclass/optimized_ikeys.c"
        "src/loclass/optimized_elite.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "src/loclass"
    REQUIRES
        nfcal
        poom_nfc
)
