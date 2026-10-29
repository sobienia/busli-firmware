// ─────────────────────────────────────────────────────────────────────────────
// pager_ui.cpp — pager screens: compose, send, incoming messages
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// Pager — display-side state
enum PagerState { PAGER_OFF=0, PAGER_ACTIVITIES, PAGER_TIME_SEL, PAGER_FRIENDS, PAGER_INCOMING };
static PagerState    g_pager_state     = PAGER_OFF;
static int           g_pager_act       = -1;   // selected activity (0-9)
static int           g_pager_time      = -1;   // selected time slot (0-5)
static int           g_pager_hilite    = -1;   // highlighted tile/row for visual feedback
static bool          g_pager_touch_was = false;
bool                 g_pager_dirty     = false; // set on state change; cleared after draw
static int           g_pager_last_min  = -1;   // elapsed minutes shown on incoming screen
static PagerConfig   g_pager_cfg;
static PagerIncoming g_pager_incoming;

static const char* PAGER_ACTS[]  = { "Beer",   "Coffee", "Movie", "Gym",  "Run",
                                      "Bike",   "Phone",  "Dinner","Lunch","Love" };
static const char* PAGER_TIMES[] = { "5 min",  "10 min", "15 min",
                                      "30 min", "Tonight","Now"    };

void pager_ui_init() {
    config_load_pager(g_pager_cfg);
    // Inject self as loopback "Me" friend for testing (not persisted)
    if (g_pager_cfg.n_friends < PAGER_MAX_FRIENDS && !g_pager_cfg.topic.isEmpty()) {
        g_pager_cfg.friends[g_pager_cfg.n_friends].name  = "Me";
        g_pager_cfg.friends[g_pager_cfg.n_friends].topic = g_pager_cfg.topic;
        g_pager_cfg.n_friends++;
    }
    if (g_pager_cfg.enabled) fetch_task_set_pager(g_pager_cfg);   // off: never checks
    Log.printf("[Pager] %s  Topic: %s  Name: %s  Friends: %d\n", g_pager_cfg.enabled ? "On" : "Off",
                  g_pager_cfg.topic.c_str(), g_pager_cfg.name.c_str(), g_pager_cfg.n_friends);
}

bool pager_ui_active() { return g_pager_state != PAGER_OFF; }

String pager_ui_name() { return g_pager_cfg.name; }

// Long press on the brightness button: open the pager, or cancel it
// (nothing while the pager is switched off in Settings)
void pager_ui_toggle() {
    if (!g_pager_cfg.enabled) return;
    if (g_pager_state == PAGER_OFF) {
        g_pager_state  = PAGER_ACTIVITIES;
        g_pager_act    = -1;
        g_pager_time   = -1;
        g_pager_hilite = -1;
        g_pager_dirty  = true;
        fetch_task_notify_pager_activity();
        Log.println("[Pager] Opened");
    } else {
        g_pager_state = PAGER_OFF;
        display_invalidate();  // reset board draw state
        Log.println("[Pager] Cancelled");
    }
}

// ── Pager touch + send ────────────────────────────────────────────────────────

static void pager_reply(int btn_idx) {
    // btn_idx: 0=Yes 1=No 2=Later; sends reply back, no reply_topic so sender gets Close-only
    if (g_pager_incoming.reply_topic.isEmpty()) return;
    if (btn_idx < 0 || btn_idx > 2) return;
    const char* labels[] = { "Yes", "No", "Later" };
    String text = String(labels[btn_idx]);
    if (!g_pager_cfg.name.isEmpty()) { text += " (from "; text += g_pager_cfg.name; text += ")"; }
    // Embed the original question so the sender can see what they asked
    if (!g_pager_incoming.display_text.isEmpty()) {
        text += "\nre=";
        text += g_pager_incoming.display_text;
    }
    http_lock_take();
    pager_publish(g_pager_incoming.reply_topic,
                  g_pager_cfg.name.isEmpty() ? "Busli" : g_pager_cfg.name,
                  "",      // empty own_topic → no reply= line → sender sees Close-only
                  text);
    http_lock_give();
}

static void pager_send(int friend_idx) {
    if (friend_idx < 0 || friend_idx >= g_pager_cfg.n_friends) return;
    if (g_pager_act < 0) return;
    if (g_pager_act != 9 && g_pager_time < 0) return;  // time required except for Love
    const String& to_topic = g_pager_cfg.friends[friend_idx].topic;
    String text = (g_pager_act == 9)
        ? "Love"
        : (String(PAGER_ACTS[g_pager_act]) + " " + String(PAGER_TIMES[g_pager_time]) + "?");
    Log.printf("[Pager] Send to %s: %s\n",
                  g_pager_cfg.friends[friend_idx].name.c_str(), text.c_str());
    fetch_task_notify_pager_activity();
    http_lock_take();
    pager_publish(to_topic, g_pager_cfg.name.isEmpty() ? "Busli" : g_pager_cfg.name,
                  g_pager_cfg.topic, text);
    http_lock_give();
}


static void handle_pager_touch() {
    bool now_pressed  = touch_is_pressed();
    bool just_pressed = (now_pressed && !g_pager_touch_was);
    bool just_lifted  = (!now_pressed && g_pager_touch_was);
    g_pager_touch_was = now_pressed;

    int tx = touch_last_screen_x();
    int ty = touch_last_screen_y();
    int content_h = SCREEN_H - HEADER_H;

    // On press-down: highlight the touched tile immediately for instant feedback
    if (just_pressed) {
        int hilite = -1;
        switch (g_pager_state) {
            case PAGER_ACTIVITIES: {
                int col = tx * 5 / SCREEN_W;
                int row = (ty - HEADER_H) * 2 / content_h;
                if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 5)
                    hilite = row * 5 + col;
                break;
            }
            case PAGER_TIME_SEL: {
                int col = tx * 3 / SCREEN_W;
                int row = (ty - HEADER_H) * 2 / content_h;
                if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 3)
                    hilite = row * 3 + col;
                break;
            }
            case PAGER_FRIENDS: {
                int n    = g_pager_cfg.n_friends;
                int rows = (n <= 3) ? 1 : 2;
                int col  = tx * 3 / SCREEN_W;
                int row  = (ty - HEADER_H) * rows / content_h;
                if (ty >= HEADER_H && row >= 0 && row < rows && col >= 0 && col < 3) {
                    int idx = row * 3 + col;
                    if (idx < n) hilite = idx;
                }
                break;
            }
            case PAGER_INCOMING:
                if (ty >= SCREEN_H - 70) {
                    bool has_replies = !g_pager_incoming.reply_topic.isEmpty();
                    hilite = has_replies ? constrain(tx * 4 / SCREEN_W, 0, 3) : 0;
                }
                break;
            default: break;
        }
        if (hilite != g_pager_hilite) {
            g_pager_hilite = hilite;
            g_pager_dirty  = true;
        }
        return;
    }

    if (!just_lifted) return;

    switch (g_pager_state) {
        case PAGER_ACTIVITIES: {
            int col = tx * 5 / SCREEN_W;
            int row = (ty - HEADER_H) * 2 / content_h;
            if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 5) {
                g_pager_act    = row * 5 + col;
                g_pager_time   = -1;
                // Love (9) skips time selection — goes straight to friend picker
                g_pager_state  = (g_pager_act == 9) ? PAGER_FRIENDS : PAGER_TIME_SEL;
                g_pager_hilite = -1;
                g_pager_dirty  = true;
            }
            break;
        }
        case PAGER_TIME_SEL: {
            int col = tx * 3 / SCREEN_W;
            int row = (ty - HEADER_H) * 2 / content_h;
            if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 3) {
                g_pager_time   = row * 3 + col;
                g_pager_state  = PAGER_FRIENDS;
                g_pager_hilite = -1;
                g_pager_dirty  = true;
            }
            break;
        }
        case PAGER_FRIENDS: {
            int n    = g_pager_cfg.n_friends;
            int rows = (n <= 3) ? 1 : 2;
            int col  = tx * 3 / SCREEN_W;
            int row  = (ty - HEADER_H) * rows / content_h;
            if (n > 0 && ty >= HEADER_H && row >= 0 && row < rows && col >= 0 && col < 3) {
                int idx = row * 3 + col;
                if (idx < n) {
                    pager_send(idx);
                    g_pager_state = PAGER_OFF;
                    display_invalidate();
                }
            }
            break;
        }
        case PAGER_INCOMING: {
            if (ty >= SCREEN_H - 70) {
                if (!g_pager_incoming.reply_topic.isEmpty()) {
                    // This is an invite — 4 buttons: Yes/No/Later send reply; Close does nothing
                    int btn = constrain(tx * 4 / SCREEN_W, 0, 3);
                    if (btn < 3) pager_reply(btn);
                }
                // else: it's a reply message — just close, no further action
                g_pager_state = PAGER_OFF;
                display_invalidate();
            }
            break;
        }
        default: break;
    }
}

// Pager owns the screen while open: returns true if it did (loop() stops there)
bool pager_ui_tick(bool swallow_input) {
    // Only dequeue when idle — if a message is already showing, let it finish.
    // The queue holds up to 4; the next pops automatically after Close/reply.
    if (g_pager_state == PAGER_OFF) {
        PagerIncoming inc;
        if (fetch_task_get_pager_incoming(inc)) {
            g_pager_incoming  = inc;
            g_pager_state     = PAGER_INCOMING;
            g_pager_hilite    = -1;
            g_pager_last_min  = -1;
            g_pager_dirty     = true;
            fetch_task_notify_pager_activity();
            Log.printf("[Pager] Incoming from %s: %s\n",
                          inc.sender_name.c_str(), inc.display_text.c_str());
        }
    }

    // ── Pager touch + render (intercepts everything when pager is open) ──────
    if (g_pager_state != PAGER_OFF) {
        // Keep the incoming timestamp live — trigger redraw each minute
        if (g_pager_state == PAGER_INCOMING && g_pager_incoming.received_at > 0) {
            int elapsed = (int)((time(nullptr) - g_pager_incoming.received_at) / 60);
            if (elapsed != g_pager_last_min) {
                g_pager_last_min = elapsed;
                g_pager_dirty    = true;
            }
        }

        if (!swallow_input) handle_pager_touch();
        if (g_pager_dirty && display_screen_on() && !overlay_active()) {
            g_pager_dirty = false;
            switch (g_pager_state) {
                case PAGER_ACTIVITIES: {
                    // Pass empty label for Love tile so only the heart icon shows
                    const char* acts[10];
                    for (int i = 0; i < 10; i++) acts[i] = (i == 9) ? "" : PAGER_ACTS[i];
                    display_draw_pager_activities(acts, g_pager_hilite);
                    break;
                }
                case PAGER_TIME_SEL:
                    display_draw_pager_times(
                        (g_pager_act >= 0) ? PAGER_ACTS[g_pager_act] : "",
                        PAGER_TIMES, g_pager_hilite);
                    break;
                case PAGER_FRIENDS: {
                    char hdr[40];
                    if (g_pager_act == 9) {
                        snprintf(hdr, sizeof(hdr), "Send love to?");
                    } else {
                        snprintf(hdr, sizeof(hdr), "%s %s?",
                                 (g_pager_act  >= 0) ? PAGER_ACTS[g_pager_act]   : "",
                                 (g_pager_time >= 0) ? PAGER_TIMES[g_pager_time] : "");
                    }
                    const char* names[PAGER_MAX_FRIENDS];
                    for (int i = 0; i < g_pager_cfg.n_friends; i++)
                        names[i] = g_pager_cfg.friends[i].name.c_str();
                    display_draw_pager_friends(hdr, names, g_pager_cfg.n_friends, g_pager_hilite);
                    break;
                }
                case PAGER_INCOMING: {
                    // icon for the main message (invite: match activity name)
                    int icon_idx = -1;
                    for (int i = 0; i < 10; i++) {
                        if (g_pager_incoming.display_text.startsWith(PAGER_ACTS[i])) {
                            icon_idx = i; break;
                        }
                    }
                    // icon for the quoted original question (reply context)
                    int re_icon_idx = -1;
                    if (!g_pager_incoming.re_text.isEmpty()) {
                        for (int i = 0; i < 10; i++) {
                            if (g_pager_incoming.re_text.startsWith(PAGER_ACTS[i])) {
                                re_icon_idx = i; break;
                            }
                        }
                    }
                    char hdr[48];
                    if (!g_pager_incoming.sender_name.isEmpty())
                        snprintf(hdr, sizeof(hdr), "From: %s",
                                 g_pager_incoming.sender_name.c_str());
                    else
                        snprintf(hdr, sizeof(hdr), "Message");
                    bool show_replies = !g_pager_incoming.reply_topic.isEmpty();
                    display_draw_pager_incoming(hdr,
                                                g_pager_incoming.display_text.c_str(),
                                                icon_idx,
                                                g_pager_incoming.received_at,
                                                show_replies,
                                                g_pager_hilite,
                                                g_pager_incoming.re_text.c_str(),
                                                re_icon_idx);
                    break;
                }
                default: break;
            }
        }
        return true;
    }
    return false;
}
