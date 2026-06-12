#ifndef MUA_SESSION_H
#define MUA_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include <cjson/cJSON.h>

#include "mua/api/private/defs.h"

// Append-only JSONL session store. One message per line in exact wire shape
// (json.h boundary rule: cJSON flows between core modules). Line 1 is the
// header control record {"type":"session","version":1,"id":...,"created":...};
// every other line is a message record (object with a string "role") or a
// tolerated unknown control record (object with a string "type").

#define MUA_SESSION_MAX_LINE (8u << 20) // bytes, one printed message
#define MUA_SESSION_MAX_MESSAGES 4096

typedef struct SessionState SessionState;

// Creates sessions/ under the state dir, claims a fresh
// YYYYMMDDTHHMMSS_NN.jsonl with O_EXCL, and writes the header line.
SessionState *session_new(Error *err);

// Loads an existing session file. A corrupt line followed by any non-empty
// line fails the load naming it; a corrupt or unterminated tail is the
// tolerated torn-tail case (logged, skipped, healed on the next append).
SessionState *session_load(const char *path, Error *err);

// Loads the lexicographically greatest well-formed session name (fixed-width
// names make strcmp order == creation order). NULL + Validation if none.
SessionState *session_load_latest(Error *err);

// Appends one message record. OWNS msg unconditionally: it is freed on every
// failure path too. Disk write happens before the in-memory append; an I/O
// failure poisons the session and all later appends fail fast.
bool session_append(SessionState *sess, cJSON *msg, Error *err);

size_t session_message_count(const SessionState *sess);
// Borrowed; stable until session_free. NULL when idx is out of range.
const cJSON *session_message_get(const SessionState *sess, size_t idx);
// The owned conversation array, borrowed for request building.
const cJSON *session_messages(const SessionState *sess);
const char *session_id(const SessionState *sess);
void session_free(SessionState *sess);

#endif // MUA_SESSION_H
