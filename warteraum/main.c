#define HTTPSERVER_IMPL
#include "../third_party/httpserver.h/httpserver.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../third_party/json_output/json_output.h"

#include "queue.h"
#include "routing.h"
#include "form.h"
#include "scrypt.h"

#include "v1_static.h" /* static strings for v1 api */
#include "tokens.h"    /* valid api tokens */

#define LISTEN_PORT 9000

#define STATIC_HTTP_STRING(s) \
  { s, sizeof(s) - 1 }

#define JSO_STATIC_PROP(s, str) \
  jso_prop_len(s, str, sizeof(str) - 1)

#define HTTP_STRING_EQ(a, b) \
  (a.len == b.len && strncmp(a.buf, b.buf, a.len) == 0)

#define HTTP_STRING_IS(a, s) \
  (a.len == sizeof(s) - 1 && strncmp(a.buf, s, a.len) == 0)

// Global state

static struct queue flip_queue;
static struct http_server_s* server;

void cleanup(int signum) {
  if(signum == SIGTERM || signum == SIGINT) {
    queue_free(flip_queue);
    free(server);
    exit(EXIT_SUCCESS);
  } else {
    fputs("Unexpected signal\n", stderr);
  }
}

// main routing logic

enum warteraum_result {
  WARTERAUM_OK = 0,
  WARTERAUM_BAD_REQUEST = 1,
  WARTERAUM_UNAUTHORIZED = 2,
  WARTERAUM_NOT_FOUND = 3,
  WARTERAUM_INTERNAL_ERROR = 4
};

void response_error(enum warteraum_result e, bool legacy_response, http_request_t *request, http_response_t *response) {
  // response_error should never be called with
  // WARTERAUM_OK, so this is considered a error 500
  const http_string_t errors[] = {
    STATIC_HTTP_STRING("internal server error"),
    STATIC_HTTP_STRING("bad request"),
    STATIC_HTTP_STRING("unauthorized"),
    STATIC_HTTP_STRING("not found"),
    STATIC_HTTP_STRING("internal server error"),
  };

  const int codes[] = { 500, 400, 401, 404, 500 };

  if(legacy_response) {
    // /api/v1/queue/add returns a HTML response
    // we emulate this behavior, however not exactly

    http_response_status(response, codes[e]);
    http_response_header(response, "Content-Type", "text/html");
    http_response_body(response, QUEUE_ADD_V1_FAILURE, sizeof(QUEUE_ADD_V1_FAILURE) - 1);
    http_respond(request, response);
  } else {
    jso_stream s;

    jso_init_growable(&s);

    jso_object(&s);
    JSO_STATIC_PROP(&s, "error");
    jso_string_len(&s, errors[e].buf, (size_t) errors[e].len);
    jso_end_object(&s);

    http_response_status(response, codes[e]);
    http_response_header(response, "Content-Type", "application/json");
    http_response_body(response, s.data, (int) s.pos);
    http_respond(request, response);

    jso_close(&s);
  }
}

// GET /api/v1/queue
// GET /api/v2/queue
enum warteraum_result response_queue(http_request_t *request, http_response_t *response) {
  unsigned int queue_length = 0;
  jso_stream s;
  jso_init_growable(&s);

  jso_object(&s);
  JSO_STATIC_PROP(&s, "queue");
  jso_array(&s);

  queue_foreach(flip_queue, elem) {
    queue_length++;
    jso_object(&s);

    JSO_STATIC_PROP(&s, "id");
    jso_uint(&s, elem->id);

    JSO_STATIC_PROP(&s, "text");
    jso_string_len(&s, elem->string, elem->string_size);

    jso_end_object(&s);
  }

  jso_end_array(&s);

  JSO_STATIC_PROP(&s, "length");
  jso_uint(&s, queue_length);
  jso_end_object(&s);

  http_response_status(response, 200);
  http_response_header(response, "Content-Type", "application/json");
  http_response_body(response, s.data, (int) s.pos);
  http_respond(request, response);

  jso_close(&s);

  return WARTERAUM_OK;
}

// POST /api/v1/queue/add
enum warteraum_result response_queue_add_v1(http_request_t *request, http_response_t *response) {
  http_string_t field_name;
  http_string_t text;

  const struct form_token_spec request_spec[] = {
    { FORM_TOKEN_STRING, &field_name },
    { FORM_TOKEN_EQUAL_SIGN, NULL },
    { FORM_TOKEN_STRING, &text }
  };

  if(flip_queue.last->id == QUEUE_MAX_ID) {
    return WARTERAUM_INTERNAL_ERROR;
  }

  http_string_t content_type = http_request_header(request, "Content-Type");
  http_string_t method = http_request_method(request);

  if(!HTTP_STRING_IS(content_type, "application/x-www-form-urlencoded") ||
     !HTTP_STRING_IS(method, "POST")) {
    return WARTERAUM_BAD_REQUEST;
  }

  http_string_t body = http_request_body(request);

  bool parse_res = form_parse(body, request_spec, sizeof(request_spec) / sizeof(struct form_token_spec));

  if(!parse_res || !HTTP_STRING_IS(field_name, "text")) {
    return WARTERAUM_BAD_REQUEST;
  }

  char *decoded = malloc(sizeof(char) * text.len);

  if(decoded == NULL) {
    return WARTERAUM_INTERNAL_ERROR;
  }

  int decoded_len = urldecode(text, decoded, sizeof(char) * text.len);

  if(decoded_len <= 0) {
    free(decoded);
    return WARTERAUM_BAD_REQUEST;
  }

  queue_append(&flip_queue, decoded, (size_t) decoded_len);

  free(decoded);

  http_response_status(response, 200);
  http_response_header(response, "Content-Type", "text/html");
  http_response_body(response, QUEUE_ADD_V1_SUCCESS, sizeof(QUEUE_ADD_V1_SUCCESS) - 1);
  http_respond(request, response);

  return WARTERAUM_OK;
}

// DELETE /api/v1/queue/del/<id>
enum warteraum_result response_queue_del_v1(http_string_t id_str, http_request_t *request, http_response_t *response) {
  http_string_t content_type = http_request_header(request, "Content-Type");
  http_string_t method = http_request_method(request);

  if(!HTTP_STRING_IS(content_type, "application/x-www-form-urlencoded") ||
     !HTTP_STRING_IS(method, "POST")) {
    return WARTERAUM_BAD_REQUEST;
  }

  http_string_t body = http_request_body(request);
  http_string_t token;
  http_string_t field_name;

  const struct form_token_spec request_spec[] = {
    { FORM_TOKEN_STRING, &field_name },
    { FORM_TOKEN_EQUAL_SIGN, NULL },
    { FORM_TOKEN_STRING, &token }
  };

  bool parse_res = form_parse(body, request_spec, sizeof(request_spec) / sizeof(struct form_token_spec));

  if(!parse_res || !HTTP_STRING_IS(field_name, "token")) {
    return WARTERAUM_BAD_REQUEST;
  }

  uint8_t hashed[SCRYPT_OUTPUT_LEN];

  int hash_result = HASH_TOKEN(token.buf, token.len, hashed);

  if(hash_result != 0) {
    return WARTERAUM_INTERNAL_ERROR;
  }

  bool token_matches = false;
  size_t token_count = sizeof(tokens) / (sizeof(uint8_t) * SCRYPT_OUTPUT_LEN);

  for(size_t i = 0; i < token_count && !token_matches; i++) {
    token_matches = true;
    for(size_t j = 0; j < SCRYPT_OUTPUT_LEN && token_matches; j++) {
      token_matches = tokens[i][j] == hashed[j];
    }
  }

  if(!token_matches) {
    return WARTERAUM_UNAUTHORIZED;
  }

  char *id_zero_terminated = malloc(sizeof(char) * (id_str.len + 1));
  if(id_zero_terminated == NULL) {
    return WARTERAUM_INTERNAL_ERROR;
  }

  memcpy(id_zero_terminated, id_str.buf, id_str.len);
  id_zero_terminated[id_str.len] = '\0';

  errno = 0;
  unsigned long int id = strtoul(id_zero_terminated, NULL, 10);

  free(id_zero_terminated);

  // check for conversion errors
  // also abort if id is greater than max id
  if(errno != 0 || id > QUEUE_MAX_ID) {
    return WARTERAUM_BAD_REQUEST;
  }

  if(flip_queue.first == NULL || flip_queue.last == NULL) {
    return WARTERAUM_NOT_FOUND;
  }

  // don't iterate through the queue if the id is out of range
  if(flip_queue.first->id > id || flip_queue.last->id < id) {
    return WARTERAUM_NOT_FOUND;
  }

  bool found = queue_remove(&flip_queue, id);

  if(found) {
    http_response_status(response, 204);
    http_respond(request, response);
    return WARTERAUM_OK;
  } else {
    return WARTERAUM_NOT_FOUND;
  }
}

void handle_request(http_request_t *request) {
  // TODO remove this for production?
  // Sending Connection: close avoids memory leaks
  // when terminating the program
  http_request_connection(request, HTTP_CLOSE);

  struct http_response_s* response = http_response_init();

  struct http_string_s target = http_request_target(request);

  struct http_string_s *segs = NULL;
  int count = split_segments(target, &segs);

  enum warteraum_result status = WARTERAUM_NOT_FOUND;
  bool v1_html_response = false;

  if(count < 0) {
    fputs("Split failure\n", stderr);
  } else {
    if(segment_match(0, "api", segs, count)) {
      if(segment_match(1, "v1", segs, count)) {
        if(segment_match(2, "queue", segs, count)) {
          if(count == 3) {
            status = response_queue(request, response);
          } else if(segment_match_last(3, "add", segs, count)) {
            // this endpoint returns html in /api/v1
            v1_html_response = true;
            status = response_queue_add_v1(request, response);
          } else if(segment_match(3, "del", segs, count) && count == 5) {
            status = response_queue_del_v1(segs[4], request, response);
          }
        }
      } else if(segment_match(1, "v2", segs, count)) {
        if(segment_match(2, "queue", segs, count)) {
          if(count == 3) {
            status = response_queue(request, response);
          }
        }
      }
    }
  }

  free(segs);

  if(status != WARTERAUM_OK) {
    response_error(status, v1_html_response, request, response);
  }
}

int main(void) {
  queue_new(&flip_queue);

  queue_append(&flip_queue, "hello", sizeof("hello") - 1);
  queue_append(&flip_queue, "world", sizeof("world") - 1);

  signal(SIGTERM, cleanup);
  signal(SIGINT, cleanup);

  server = http_server_init(LISTEN_PORT, handle_request);
  http_server_listen(server);

  return 0;
}
