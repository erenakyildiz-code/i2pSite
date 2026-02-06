#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_MESSAGES 50

typedef struct {
  char user[64];
  char text[300];
  char time_str[32];
} ChatMessage;

ChatMessage messages[MAX_MESSAGES];
int message_count = 0;
pthread_mutex_t chat_mutex = PTHREAD_MUTEX_INITIALIZER;

void url_decode(char *dst, const char *src) {
  char a, b;
  while (*src) {
    if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
        (isxdigit(a) && isxdigit(b))) {
      if (a >= 'a')
        a -= 'a' - 'A';
      if (a >= 'A')
        a -= ('A' - 10);
      else
        a -= '0';
      if (b >= 'a')
        b -= 'a' - 'A';
      if (b >= 'A')
        b -= ('A' - 10);
      else
        b -= '0';
      *dst++ = 16 * a + b;
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst++ = '\0';
}

void html_escape(char *dst, const char *src, size_t dst_size) {
  size_t n = 0;
  while (*src && n < dst_size - 1) {
    if (*src == '<') {
      if (n + 4 >= dst_size)
        break;
      strcpy(dst + n, "&lt;");
      n += 4;
    } else if (*src == '>') {
      if (n + 4 >= dst_size)
        break;
      strcpy(dst + n, "&gt;");
      n += 4;
    } else if (*src == '&') {
      if (n + 5 >= dst_size)
        break;
      strcpy(dst + n, "&amp;");
      n += 5;
    } else {
      dst[n++] = *src;
    }
    src++;
  }
  dst[n] = '\0';
}

void handle_client(int client_socket) {
  char buffer[BUFFER_SIZE];
  int read_size = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
  if (read_size < 0) {
    close(client_socket);
    return;
  }
  buffer[read_size] = '\0';

  char method[16] = {0}, path[256] = {0};
  // Fix: Add width limits to prevent stack overflow
  sscanf(buffer, "%15s %255s", method, path);
  printf("Request: %s %s\n", method, path);

  // Security: Prevent Directory Traversal
  if (strstr(path, "..")) {
    char *response = "HTTP/1.1 403 Forbidden\r\n\r\nForbidden";
    send(client_socket, response, strlen(response), 0);
    close(client_socket);
    return;
  }

  if (strcmp(method, "POST") == 0 && strcmp(path, "/chat") == 0) {
    printf("Processing POST /chat\n");
    // Handle Chat Submission
    char *body = strstr(buffer, "\r\n\r\n");
    if (body) {
      body += 4;
      // printf("Body found: %s\n", body); // Disable body logging for
      // privacy/noise
      char user[256] = "Anonymous";
      char text[1024] = "";

      // Very simple form parsing (user=...&text=...)
      char *user_ptr = strstr(body, "username=");
      char *text_ptr = strstr(body, "message=");

      if (text_ptr) {
        char temp_text[1024] = {0};
        // Read until &, or end of string.
        // Using a loop or strcspn is safer than sscanf for this
        size_t len = strcspn(text_ptr + 8, "&");
        if (len > 1023)
          len = 1023;
        strncpy(temp_text, text_ptr + 8, len);
        temp_text[len] = '\0';

        url_decode(text, temp_text);
      }
      if (user_ptr) {
        char temp_user[256] = {0};
        size_t len = strcspn(user_ptr + 9, "&");
        if (len > 255)
          len = 255;
        strncpy(temp_user, user_ptr + 9, len);
        temp_user[len] = '\0';

        url_decode(user, temp_user);
      }

      printf("Parsed - User: %s, Text lengths: %lu\n", user, strlen(text));

      if (strlen(text) > 0) {
        pthread_mutex_lock(&chat_mutex);

        // Shift if full
        if (message_count < MAX_MESSAGES) {
          message_count++;
        } else {
          for (int i = 0; i < MAX_MESSAGES - 1; i++)
            messages[i] = messages[i + 1];
        }

        int idx = message_count - 1;
        html_escape(messages[idx].user, user, 64);
        html_escape(messages[idx].text, text, 300);

        time_t now = time(NULL);
        strftime(messages[idx].time_str, sizeof(messages[idx].time_str),
                 "%H:%M", localtime(&now));

        pthread_mutex_unlock(&chat_mutex);
      }
    } else {
      printf("No body found in request\n");
    }

    // Redirect back to home
    char *response = "HTTP/1.1 303 See Other\r\nLocation: /\r\n\r\n";
    send(client_socket, response, strlen(response), 0);

    // Keep socket open briefly to ensure flush (simple hack) or just shutdown
    shutdown(client_socket, SHUT_WR);
    close(client_socket);
    return;
  }

  // Serve Files (SSR for index.html)
  char file_path[512];
  if (strcmp(path, "/") == 0) {
    snprintf(file_path, sizeof(file_path), "wwwroot/index.html");
  } else {
    snprintf(file_path, sizeof(file_path), "wwwroot%s", path);
  }

  FILE *f = fopen(file_path, "r");
  if (!f) {
    char *response = "HTTP/1.1 404 Not Found\r\n\r\nNot Found";
    send(client_socket, response, strlen(response), 0);
  } else {
    // Simple SSR: Read file into memory, inject messages if marker found
    // Note: For production C this should be more robust, here we load small
    // files
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *html = malloc(fsize + 1);
    fread(html, 1, fsize, f);
    fclose(f);
    html[fsize] = 0;

    char *header = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    if (strstr(path, ".css"))
      header = "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n\r\n";

    send(client_socket, header, strlen(header), 0);

    if (strstr(file_path, "index.html")) {
      char *marker = "<!-- CHAT_MESSAGES -->";
      char *pos = strstr(html, marker);
      if (pos) {
        // Send part before marker
        send(client_socket, html, pos - html, 0);

        // Send messages
        pthread_mutex_lock(&chat_mutex);
        for (int i = 0; i < message_count; i++) {
          char msg_html[512];
          snprintf(msg_html, sizeof(msg_html),
                   "<div class=\"message\"><span class=\"time\">[%s]</span> "
                   "<span class=\"user\">%s:</span> <span "
                   "class=\"text\">%s</span></div>\n",
                   messages[i].time_str, messages[i].user, messages[i].text);
          send(client_socket, msg_html, strlen(msg_html), 0);
        }
        pthread_mutex_unlock(&chat_mutex);

        // Send part after marker
        send(client_socket, pos + strlen(marker), strlen(pos + strlen(marker)),
             0);
      } else {
        send(client_socket, html, fsize, 0);
      }
    } else {
      send(client_socket, html, fsize, 0);
    }
    free(html);
  }
  close(client_socket);
}

#define RATE_LIMIT_WINDOW 1 // seconds
#define MAX_REQUESTS_PER_WINDOW 20
time_t current_window_start = 0;
int request_count = 0;
pthread_mutex_t rate_mutex = PTHREAD_MUTEX_INITIALIZER;

int check_rate_limit() {
  time_t now = time(NULL);
  int allowed = 1;

  pthread_mutex_lock(&rate_mutex);
  if (now > current_window_start + RATE_LIMIT_WINDOW) {
    current_window_start = now;
    request_count = 0;
  }

  if (request_count >= MAX_REQUESTS_PER_WINDOW) {
    allowed = 0;
  } else {
    request_count++;
  }
  pthread_mutex_unlock(&rate_mutex);
  return allowed;
}

int main() {
  int server_sock = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
             sizeof(opt));

  // Disable output buffering to see logs immediately
  setbuf(stdout, NULL);

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
  listen(server_sock, 10);

  printf("C Server running on 8080...\n");
  printf("Rate Limit: %d requests / %d sec\n", MAX_REQUESTS_PER_WINDOW,
         RATE_LIMIT_WINDOW);

  while (1) {
    int client_sock = accept(server_sock, NULL, NULL);
    if (client_sock >= 0) {
      if (!check_rate_limit()) {
        char *resp = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: "
                     "1\r\n\r\nServer is busy.";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        continue;
      }

      pthread_t t;
      // For simplicity in this demo, passing int directly (int is safe on 64bit
      // usually but struct is better)
      // Warning: handling race on client_sock processing in heavy load, but
      // fine here
      pthread_create(&t, NULL, (void *(*)(void *))handle_client,
                     (void *)(long)client_sock);
      pthread_detach(t);
    }
  }
  return 0;
}
