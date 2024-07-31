#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

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

#define IPC_HEADER_SIZE (sizeof(ipc_magic) + 8)
#define EXP_LOG_FN "expose.log"
#define event_mask(ev) (1 << (ev & 0x7F))
#define log(...)                                                               \
  if (log) {                                                                   \
    struct tm tm = *localtime(&(time_t){time(NULL)});                          \
    char buf[9];                                                               \
    strftime(buf, sizeof(buf), "%T", &tm);                                     \
    fprintf(log_fp, "[%s] ", buf);                                             \
    fprintf(log_fp, __VA_ARGS__);                                              \
    fprintf(log_fp, "\n");                                                     \
  }
#define abort(...)                                                             \
  do {                                                                         \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

char *get_socketpath(void) {
  const char *swaysock = getenv("SWAYSOCK");
  if (swaysock) {
    return strdup(swaysock);
  }
  char *line = NULL;
  size_t line_size = 0;
  FILE *fp = popen("sway --get-socketpath 2>/dev/null", "r");
  if (fp) {
    ssize_t nret = getline(&line, &line_size, fp);
    pclose(fp);
    if (nret > 0) {
      if (line[nret - 1] == '\n') {
        line[nret - 1] = '\0';
      }
      return line;
    }
  }
  return NULL;
}

int ipc_open_socket(const char *socket_path) {
  struct sockaddr_un addr;
  int socketfd;
  if ((socketfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    abort("Unable to open Unix socket");
  }
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
  int l = sizeof(struct sockaddr_un);
  if (connect(socketfd, (struct sockaddr *)&addr, l) == -1) {
    abort("Unable to connect to %s", socket_path);
  }
  return socketfd;
}

bool ipc_set_recv_timeout(int socketfd, struct timeval tv) {
  if (setsockopt(socketfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    abort("Failed to set ipc recv timeout");
    return false;
  }
  return true;
}

struct ipc_response *ipc_recv_response(int socketfd) {
  char data[IPC_HEADER_SIZE];

  size_t total = 0;
  while (total < IPC_HEADER_SIZE) {
    ssize_t received = recv(socketfd, data + total, IPC_HEADER_SIZE - total, 0);
    if (received <= 0) {
      abort("Unable to receive IPC response");
    }
    total += received;
  }

  struct ipc_response *response = malloc(sizeof(struct ipc_response));
  if (!response) {
    goto error_1;
  }

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
    if (received < 0) {
      abort("Unable to receive IPC response");
    }
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

  if (write(socketfd, data, IPC_HEADER_SIZE) == -1) {
    abort("Unable to send IPC header");
  }

  if (write(socketfd, payload, *len) == -1) {
    abort("Unable to send IPC payload");
  }

  struct ipc_response *resp = ipc_recv_response(socketfd);
  char *response = resp->payload;
  *len = resp->size;
  free(resp);

  return response;
}

int main(int argc, char **argv) {
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
    strcat(strcat(log_fn, getenv("EXPOSWAYDIR")), EXP_LOG_FN);
    log_fp = fopen(log_fn, "w");
    free(log_fn);
  }

  char *socket_path;
  socket_path = get_socketpath();
  if (!socket_path)
    abort("Unable to retrieve socket path");

  uint32_t type = IPC_COMMAND;
  char *command = NULL;

  int ret = 0;
  int socketfd = ipc_open_socket(socket_path);
  struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
  ipc_set_recv_timeout(socketfd, timeout);

  type = IPC_GET_OUTPUTS;
  command = strdup("");
  uint32_t len = strlen(command);
  char *resp = ipc_single_command(socketfd, type, command, &len);
  printf(resp);

  free(command);
  free(resp);

  log("Exposway daemon initialized successfully.");

  if (log)
    fclose(log_fp);

  close(socketfd);
  free(socket_path);

  return 0;
}
