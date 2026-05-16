#pragma once
#include <Arduino.h>
#include <vector>

#define PAGER_MAX_FRIENDS  5
#define PAGER_POLL_SEC     30

struct PagerFriend {
    String name;
    String topic;
};

struct PagerConfig {
    String      name;                        // this device's display name
    String      topic;                       // this device's ntfy.sh topic
    PagerFriend friends[PAGER_MAX_FRIENDS];
    int         n_friends = 0;
};

struct PagerIncoming {
    String  display_text;  // first line — what to show on screen
    String  sender_name;   // "title" field from ntfy.sh; may be empty
    String  reply_topic;   // extracted "reply=..." line; may be empty
    String  re_text;       // extracted "re=..." line; original question for reply context
    time_t  received_at;
};

// Generate a random "busli-XXXXXXXXXXXXXXXX" topic from the hardware RNG.
String pager_generate_topic();

// POST a message to a contact's ntfy.sh topic.
// own_topic is embedded in the body so the recipient can reply.
bool pager_publish(const String& to_topic,
                   const String& sender_name,
                   const String& own_topic,
                   const String& text);

// Poll own topic for new messages since last_poll_time (updated on success).
bool pager_poll(const String& own_topic,
                time_t&       last_poll_time,
                std::vector<PagerIncoming>& out_new);
