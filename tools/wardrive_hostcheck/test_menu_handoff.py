"""Execute the actual menu handoff functions with mocked SBUS/task services.

This tests the entry table and ownership lifecycle, not physical OLED rendering.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

source = Path('applications/poom_menu/src/poom_menu.c').read_text()
assert re.search(r'\{"WARDRIVE",\s*action_wardrive_\}', source)
assert not re.search(r'\{"WARDRIVE",\s*menu_wardrive_show\}', source)

def function(name):
    match = re.search(r'static void ' + name + r'\([^;{}]*\)\s*\{', source)
    assert match, name
    start = match.start()
    cursor = match.end()
    depth = 1
    while depth:
        if source[cursor] == '{':
            depth += 1
        elif source[cursor] == '}':
            depth -= 1
        cursor += 1
    return source[start:cursor]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_IDF_TARGET_ESP32C5 1
#define pdMS_TO_TICKS(x) (x)
typedef struct { int unused; } poom_sbus_msg_t;
static bool s_detached, s_input_subscribed = true;
static unsigned subscribed, unsubscribed, suspended, resumed, opened, reset;
static void on_button_any_(const poom_sbus_msg_t *m, void *u) { (void)m; (void)u; }
static int poom_sbus_unsubscribe_cb(const char *topic, void (*cb)(const poom_sbus_msg_t *,void *), void *ctx) {
    assert(!strcmp(topic,"input/button") && cb==on_button_any_ && ctx==NULL);
    unsubscribed++; return 0;
}
static int poom_sbus_subscribe_cb(const char *topic, void (*cb)(const poom_sbus_msg_t *,void *), void *ctx) {
    assert(!strcmp(topic,"input/button") && cb==on_button_any_ && ctx==NULL);
    subscribed++; return 0;
}
static void poom_button_sound_suspend(void) { suspended++; }
static void poom_button_sound_resume(void) { resumed++; }
static void power_reset_idle_(void) { reset++; }
static void vTaskDelay(unsigned ticks) {
    assert(ticks==180 && s_detached && !s_input_subscribed);
}
static void menu_wardrive_show(void) {
    assert(s_detached && !s_input_subscribed);
    opened++;
}
'''
harness += '\n'.join(function(n) for n in (
    'detach_menu_', 'on_menu_resume_', 'action_wardrive_'))
harness += r'''
int main(void) {
    for(unsigned i=1; i<=3; i++) {
        action_wardrive_();
        assert(opened==i && unsubscribed==i && suspended==i);
        assert(s_detached && !s_input_subscribed);
        on_menu_resume_(NULL,NULL);
        assert(!s_detached && s_input_subscribed);
        assert(subscribed==i && resumed==i && reset==i);
        on_menu_resume_(NULL,NULL); /* duplicate notification is harmless */
        assert(subscribed==i && resumed==i);
    }
    puts("Menu handoff: actual Wardrive action, display/button ownership and repeated resume passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wardrive-menu-') as tmp:
    c = Path(tmp) / 'handoff.c'
    exe = Path(tmp) / 'handoff'
    c.write_text(harness)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)
    ], check=True)
    subprocess.run([str(exe)], check=True)
