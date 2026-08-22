set(POOM_WEB_ENABLE_TONE_PAGE 0)
set(POOM_WEB_ENABLE_MIDI_PAGE 0)

set(POOM_WEB_SRCS
    "poom_web.c"
    "modules/http_server/poom_web_http_server.c"
    "modules/http_server/poom_web_ir_session.c"
)

set(POOM_WEB_INCLUDE_DIRS
    "include"
    "modules/http_server"
)

set(POOM_WEB_PRIV_REQUIRES
    esp_http_server
    esp_timer
    poom_wifi_ctrl
    mdns_manager
    poom_secrets_store
    sd_card
    cjson
    board
    ir
)

set(POOM_WEB_EMBED_FILES
    src/webpage/poom_cli_web_index.html
    src/webpage/poom_cli_web_app.css
    src/webpage/poom_cli_web_app.js
    src/webpage/poom_cli_web_ir.html
    src/webpage/poom_cli_web_ir.css
    src/webpage/poom_cli_web_ir.js
)

if(POOM_WEB_ENABLE_TONE_PAGE)
    list(APPEND POOM_WEB_PRIV_REQUIRES
        buzzer
        poom_buz_theme
    )
    list(APPEND POOM_WEB_EMBED_FILES
        src/webpage/poom_cli_web_tone.html
        src/webpage/poom_cli_web_tone.css
        src/webpage/poom_cli_web_tone.js
    )
endif()

if(POOM_WEB_ENABLE_MIDI_PAGE)
    list(APPEND POOM_WEB_SRCS
        "modules/web_midi/poom_web_midi.c"
    )
    list(APPEND POOM_WEB_INCLUDE_DIRS
        "modules/web_midi"
    )
    list(APPEND POOM_WEB_PRIV_REQUIRES
        poom_midi
    )
    list(APPEND POOM_WEB_EMBED_FILES
        src/webpage/poom_cli_web_midi.html
        src/webpage/poom_cli_web_midi.css
        src/webpage/poom_cli_web_midi.js
    )
endif()

idf_component_register(
    SRCS ${POOM_WEB_SRCS}
    INCLUDE_DIRS ${POOM_WEB_INCLUDE_DIRS}
    PRIV_REQUIRES ${POOM_WEB_PRIV_REQUIRES}
    EMBED_FILES ${POOM_WEB_EMBED_FILES}
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    POOM_WEB_ENABLE_TONE_PAGE=${POOM_WEB_ENABLE_TONE_PAGE}
    POOM_WEB_ENABLE_MIDI_PAGE=${POOM_WEB_ENABLE_MIDI_PAGE}
)
