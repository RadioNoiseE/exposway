#include <json.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stop = 0;

static const char ipc_magic[] = {'i', '3', '-', 'i', 'p', 'c'};
enum ipc_command_type {
  IPC_COMMAND = 0,
  IPC_GET_WORKSPACES = 1,
  IPC_SUBSCRIBE = 2,
  IPC_GET_OUTPUTS = 3,
  IPC_GET_TREE = 4,
  IPC_GET_MARKS = 5,
  IPC_GET_BAR_CONFIG = 6,
  IPC_GET_VERSION = 7,
  IPC_GET_BINDING_MODES = 8,
  IPC_GET_CONFIG = 9,
  IPC_SEND_TICK = 10,
  IPC_SYNC = 11,
  IPC_GET_BINDING_STATE = 12,
  IPC_GET_INPUTS = 100,
  IPC_GET_SEATS = 101,
  IPC_EVENT_WORKSPACE = ((1 << 31) | 0),
  IPC_EVENT_OUTPUT = ((1 << 31) | 1),
  IPC_EVENT_MODE = ((1 << 31) | 2),
  IPC_EVENT_WINDOW = ((1 << 31) | 3),
  IPC_EVENT_BARCONFIG_UPDATE = ((1 << 31) | 4),
  IPC_EVENT_BINDING = ((1 << 31) | 5),
  IPC_EVENT_SHUTDOWN = ((1 << 31) | 6),
  IPC_EVENT_TICK = ((1 << 31) | 7),
  IPC_EVENT_BAR_STATE_UPDATE = ((1 << 31) | 20),
  IPC_EVENT_INPUT = ((1 << 31) | 21),
};
struct ipc_response {
  uint32_t size;
  uint32_t type;
  char *payload;
};
bool termina;

#define IPC_HEADER_SIZE (sizeof(ipc_magic) + 8)
#define EXP_LOG_FN "expose.log"
#define EXP_MON_FN "output"
#define EXP_SUB_PL "[\"window\"]"
#define JSON_MAX_DEPTH 124
#define event_mask(ev) (1 << (ev & 0x7F))
#define log(...)                                                               \
  if (log) {                                                                   \
    struct tm tm = *localtime(&(time_t){time(NULL)});                          \
    char buf[9];                                                               \
    strftime(buf, sizeof(buf), "%T", &tm);                                     \
    fprintf(log_fp, "[%s] ", buf);                                             \
    fprintf(log_fp, __VA_ARGS__);                                              \
    fprintf(log_fp, "\n");                                                     \
    fflush(log_fp);                                                            \
  }
#define abort(...)                                                             \
  do {                                                                         \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

char *get_socketpath(void) {
  const char *swaysock = getenv("SWAYSOCK");
  if (swaysock)
    return strdup(swaysock);
  char *line = NULL;
  size_t line_size = 0;
  FILE *fp = popen("sway --get-socketpath 2>/dev/null", "r");
  if (fp) {
    ssize_t nret = getline(&line, &line_size, fp);
    pclose(fp);
    if (nret > 0) {
      if (line[nret - 1] == '\n')
        line[nret - 1] = '\0';
      return line;
    }
  }
  return NULL;
}

int ipc_open_socket(const char *socket_path) {
  struct sockaddr_un addr;
  int socketfd;
  if ((socketfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    abort("Unable to open Unix socket");
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
  int l = sizeof(struct sockaddr_un);
  if (connect(socketfd, (struct sockaddr *)&addr, l) == -1)
    abort("Unable to connect to %s", socket_path);
  return socketfd;
}

bool ipc_set_recv_timeout(int socketfd, struct timeval tv) {
  if (setsockopt(socketfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
    return false;
  return true;
}

struct ipc_response *ipc_recv_response(int socketfd) {
  char data[IPC_HEADER_SIZE];

  size_t total = 0;
  while (total < IPC_HEADER_SIZE) {
    ssize_t received = recv(socketfd, data + total, IPC_HEADER_SIZE - total, 0);
    if (received <= 0)
      abort("Unable to receive IPC response");
    total += received;
  }

  struct ipc_response *response = malloc(sizeof(struct ipc_response));
  if (!response)
    goto error_1;

  memcpy(&response->size, data + sizeof(ipc_magic), sizeof(uint32_t));
  memcpy(&response->type, data + sizeof(ipc_magic) + sizeof(uint32_t),
         sizeof(uint32_t));

  char *payload = malloc(response->size + 1);
  if (!payload) {
    goto error_2;
  }

  total = 0;
  while (total < response->size) {
    ssize_t received =
        recv(socketfd, payload + total, response->size - total, 0);
    if (received < 0)
      abort("Unable to receive IPC response");
    total += received;
  }
  payload[response->size] = '\0';
  response->payload = payload;

  return response;
error_2:
  free(response);
error_1:
  abort("Unable to allocate memory for IPC response");
  return NULL;
}

void free_ipc_response(struct ipc_response *response) {
  free(response->payload);
  free(response);
}

char *ipc_single_command(int socketfd, uint32_t type, const char *payload,
                         uint32_t *len) {
  char data[IPC_HEADER_SIZE];
  memcpy(data, ipc_magic, sizeof(ipc_magic));
  memcpy(data + sizeof(ipc_magic), len, sizeof(*len));
  memcpy(data + sizeof(ipc_magic) + sizeof(*len), &type, sizeof(type));

  if (write(socketfd, data, IPC_HEADER_SIZE) == -1)
    abort("Unable to send IPC header");

  if (write(socketfd, payload, *len) == -1)
    abort("Unable to send IPC payload");

  struct ipc_response *resp = ipc_recv_response(socketfd);
  char *response = resp->payload;
  *len = resp->size;
  free(resp);

  return response;
}

void garbage_collect(int sig) { termina = 0; }

int main(int argc, char **argv) {
  signal(SIGTERM, garbage_collect);

  termina = 1;
  bool log = false;

  if (getenv("EXPOSWAYDIR") == NULL)
    abort("Unset curcial environment variable");

  if (argc >= 3)
    abort("Too many arguments");
  if (argc == 2 && strcmp(*++argv, "-l") == 0)
    log = true;

  int ret_code;
  char *cache_cmd = malloc(
      (strlen("find % -mindepth 1 -delete") + strlen(getenv("EXPOSWAYDIR"))) *
      sizeof(char));
  sprintf(cache_cmd, "find %s -mindepth 1 -delete", getenv("EXPOSWAYDIR"));
  if ((ret_code = system(cache_cmd)) != 0) {
    free(cache_cmd);
    abort("Syscall failed with return code %d", ret_code);
  }
  free(cache_cmd);

  char *log_fn = NULL;
  FILE *log_fp = NULL;
  if (log) {
    log_fn = malloc((strlen(getenv("EXPOSWAYDIR")) + strlen(EXP_LOG_FN) + 1) *
                    sizeof(char));
    strcat(strcpy(log_fn, getenv("EXPOSWAYDIR")), EXP_LOG_FN);
    log_fp = fopen(log_fn, "w");
    free(log_fn);
  }

  log("Exposway daemon initialized successfully.");

  char *socket_path = get_socketpath();
  if (!socket_path)
    abort("Unable to retrieve socket path");

  log("Unix socket for swayWM IPC protocol retrieved.");

  uint32_t type = IPC_COMMAND;
  char *command = NULL;

  int socket_fd = ipc_open_socket(socket_path);

  log("Connection established.");

  struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
  ipc_set_recv_timeout(socket_fd, timeout);

  type = IPC_GET_OUTPUTS;
  command = strdup("");
  uint32_t len = strlen(command);
  char *resp = ipc_single_command(socket_fd, type, command, &len);

  log("Output specification request sent.");

  json_tokener *tok = json_tokener_new_ex(JSON_MAX_DEPTH);
  if (tok == NULL)
    abort("Failed allocating json_tokener");
  json_object *obj = json_tokener_parse_ex(tok, resp, -1);
  enum json_tokener_error err = json_tokener_get_error(tok);
  json_tokener_free(tok);
  if (obj == NULL || err != json_tokener_success)
    abort("Failed to parse payload as json: %s", json_tokener_error_desc(err));

  int array_len = json_object_array_length(obj);
  for (int i = 0; i < array_len; i++) {
    json_object *element = json_object_array_get_idx(obj, i);
    json_object *focused;

    if (json_object_object_get_ex(element, "focused", &focused) &&
        json_object_get_boolean(focused)) {
      json_object *display, *geometry_width, *geometry_height;
      json_object_object_get_ex(element, "rect", &display);
      json_object_object_get_ex(display, "width", &geometry_width);
      json_object_object_get_ex(display, "height", &geometry_height);

      char *mon_fn =
          malloc((strlen(getenv("EXPOSWAYDIR")) + strlen(EXP_MON_FN) + 1) *
                 sizeof(char));
      strcat(strcpy(mon_fn, getenv("EXPOSWAYDIR")), EXP_MON_FN);
      FILE *mon_fp = fopen(mon_fn, "w");
      fprintf(mon_fp, "%d %d", json_object_get_int(geometry_width),
              json_object_get_int(geometry_height));
      fclose(mon_fp);
      free(mon_fn);
    }
  }

  log("Currently focused monitor's geometry parsed and written.");

  json_object_put(obj);
  free(command);
  free(resp);

  len = strlen(EXP_SUB_PL);
  ipc_single_command(socket_fd, IPC_SUBSCRIBE, EXP_SUB_PL, &len);

  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  ipc_set_recv_timeout(socket_fd, timeout);

  do {
    struct ipc_response *reply = ipc_recv_response(socket_fd);
    if (!reply)
      break;

    json_tokener *tok = json_tokener_new_ex(JSON_MAX_DEPTH);
    if (tok == NULL)
      abort("Failed allocating json_tokener");

    json_object *obj = json_tokener_parse_ex(tok, reply->payload, -1);
    enum json_tokener_error err = json_tokener_get_error(tok);
    json_tokener_free(tok);
    if (obj == NULL || err != json_tokener_success)
      abort("Failed to parse payload as json: %s",
            json_tokener_error_desc(err));

    json_object *cont;
    json_object_object_get_ex(obj, "container", &cont);

    json_object *stat, *ref;
    json_object_object_get_ex(obj, "change", &stat);
    json_object_object_get_ex(cont, "name", &ref);

    const char *title = json_object_get_string(ref);
    if (strcmp("Sway Expose", title) != 0) {
      json_object *focused;
      if (json_object_object_get_ex(cont, "focused", &focused) &&
          json_object_get_boolean(focused)) {
        const char *state = json_object_get_string(stat);
        if (strcmp("focus", state) == 0 || strcmp("title", state) == 0 ||
            strcmp("move", state) == 0 ||
            strcmp("fullscreen_mode", state) == 0 ||
            strcmp("floating", state) == 0) {
          json_object *node, *rect;
          json_object *xcr, *ycr, *width, *height;

          json_object_object_get_ex(cont, "id", &node);
          int uid = json_object_get_int(node);
          char uuid[17];
          snprintf(uuid, 16, "%d", uid);

          json_object_object_get_ex(cont, "rect", &rect);
          json_object_object_get_ex(rect, "x", &xcr);
          json_object_object_get_ex(rect, "y", &ycr);
          json_object_object_get_ex(rect, "width", &width);
          json_object_object_get_ex(rect, "height", &height);
          int x = json_object_get_int(xcr);
          int y = json_object_get_int(ycr);
          int wd = json_object_get_int(width);
          int ht = json_object_get_int(height);

          log("Window %d (%s) with changed mode (%s) detected, with coordinate "
              "(%d,%d) and geometry %dx%d",
              uid, title, state, x, y, wd, ht);

          char *win_fn =
              malloc(strlen(getenv("EXPOSWAYDIR")) + strlen(uuid) + 1);
          strcat(strcpy(win_fn, getenv("EXPOSWAYDIR")), uuid);
          FILE *win_fp = fopen(win_fn, "w");
          free(win_fn);
          fprintf(win_fp, "%d,%d %dx%d %s", x, y, wd, ht, title);
          fclose(win_fp);

          char grim[72];
          snprintf(grim, 72, "grim -g \"%d,%d %dx%d\" %s%d.png", x, y, wd, ht,
                   getenv("EXPOSWAYDIR"), uid);
          system(grim);
        } else if (strcmp("close", state)) {
          json_object *node;

          json_object_object_get_ex(cont, "id", &node);
          int uid = json_object_get_int(node);
          char uuid[17];
          snprintf(uuid, 16, "%d", uid);

          log("Window %d closed, deleting cache.", uid);

          char *win_fn =
              malloc(strlen(getenv("EXPOSWAYDIR")) + strlen(uuid) + 5);
          strcat(strcpy(win_fn, getenv("EXPOSWAYDIR")), uuid);
          unlink(win_fn);
          strcat(win_fn, ".png");
          unlink(win_fn);
          free(win_fn);
        }
      }
    }

    json_object_put(obj);

    free_ipc_response(reply);
  } while (termina);

  log("Terminate signal caught, cleaning up.");

  if (log)
    fclose(log_fp);

  close(socket_fd);
  free(socket_path);

  return 0;
}
