# poom_clipper

`poom_clipper` is a small helper module for reading Clipper transit-card data over an active NFC ISO-DEP / DESFire session.

It focuses on Clipper-specific command wrapping and history decoding rather than generic NFC transport.

## Structure

```text
modules/poom_clipper
├── CMakeLists.txt
├── README.md
├── poom_clipper.c
└── include/
    └── poom_clipper.h
```

## Dependencies

Declared in `modules/poom_clipper/CMakeLists.txt`:

Private requirements:

* `poom_nfc`

## Public API

Header:
`modules/poom_clipper/include/poom_clipper.h`

```c
bool poom_clipper_desfire_cmd_collect(uint8_t ins,
                                      const uint8_t* data,
                                      size_t data_len,
                                      uint8_t* out_buf,
                                      size_t out_buf_max,
                                      size_t* out_len,
                                      uint8_t* out_status);
int poom_clipper_print_history(void);
```

## Runtime Behavior

The module expects an already active NFC reader + ISO-DEP connection.

From there it can:

* send wrapped DESFire native commands,
* collect chained responses,
* parse Clipper-specific records,
* print decoded ride-history information to the console.

## Integration

* This is primarily a protocol helper for NFC workflows and CLI/debug tooling.
* It is not a standalone menu app by itself.
