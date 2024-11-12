#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

// 定義 ANSI 顏色代碼（可選，用於提示符美化）
#define COLOR_RESET   "\x1b[0m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RED     "\x1b[31m"

// 內建指令列表
const char *built_in_commands[] = {"cd", "exit", "echo", "help", "history", "export", NULL};

// 信號處理程序，用於處理 SIGINT (Ctrl+C)
void sigint_handler(int sig) {
    // 在接收到 SIGINT 時，清除當前輸入並顯示新的提示符
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

// 擴展 tilde (~) 為用戶主目錄
char *tilde_expansion(const char *path) {
    if (path[0] != '~') {
        return strdup(path);
    }

    const char *home_dir = getenv("HOME");
    if (!home_dir) {
        struct passwd *pw = getpwuid(getuid());
        if (pw)
            home_dir = pw->pw_dir;
        else
            home_dir = "";
    }

    // 如果 path 是 "~" 或以 "~/" 開頭
    if (strcmp(path, "~") == 0) {
        return strdup(home_dir);
    } else if (path[1] == '/') {
        size_t len = strlen(home_dir) + strlen(path);
        char *expanded = malloc(len + 1);
        if (!expanded) {
            perror("malloc failed");
            return NULL;
        }
        strcpy(expanded, home_dir);
        strcat(expanded, path + 1); // 跳過 '~'
        return expanded;
    }

    // 其他情況（如 ~user），暫不處理，返回原路徑
    return strdup(path);
}

// 擴展歷史命令 !! 為上一條命令
char *expand_history(const char *input) {
    // 確定是否存在 '!!'
    char *pos = strstr(input, "!!");
    if (!pos) {
        return strdup(input); // 無需擴展
    }

    HIST_ENTRY **hist_list = history_list();
    if (!hist_list || history_length == 0) {
        fprintf(stderr, "No commands in history.\n");
        return strdup(input); // 無法擴展，返回原輸入
    }

    const char *last_command = hist_list[history_length - 1]->line;

    // 計算擴展後的字串長度
    size_t new_len = strlen(input) - 2 + strlen(last_command) + 1;
    char *result = malloc(new_len);
    if (!result) {
        perror("malloc failed");
        return NULL;
    }

    // 替換 '!!' 為 last_command
    strncpy(result, input, pos - input);
    result[pos - input] = '\0';
    strcat(result, last_command);
    strcat(result, pos + 2); // 跳過 '!!'

    return result;
}

// 判斷是否為內建指令
int is_built_in(const char *cmd_name) {
    for (int i = 0; built_in_commands[i] != NULL; i++) {
        if (strcmp(cmd_name, built_in_commands[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// 內建指令處理函數
int handle_built_in(char **args) {
    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL) {
            const char *home_dir = getenv("HOME");
            if (home_dir == NULL) {
                struct passwd *pw = getpwuid(getuid());
                home_dir = pw->pw_dir;
            }
            if (chdir(home_dir) != 0) {
                perror("cd");
            }
        } else {
            if (chdir(args[1]) != 0) {
                perror("cd");
            }
        }
        return 1;
    }

    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }

    if (strcmp(args[0], "echo") == 0) {
        for (int i = 1; args[i] != NULL; i++) {
            printf("%s ", args[i]);
        }
        printf("\n");
        return 1;
    }

    if (strcmp(args[0], "help") == 0) {
        printf("Simple Shell Built-in Commands:\n");
        printf("  cd [dir]        Change the current directory to 'dir'.\n");
        printf("  exit            Exit the shell.\n");
        printf("  echo [args]     Display the given arguments.\n");
        printf("  help            Display this help message.\n");
        printf("  history         Show command history.\n");
        printf("  export VAR=val  Set environment variable VAR to val.\n");
        return 1;
    }

    if (strcmp(args[0], "history") == 0) {
        HIST_ENTRY **hist_list = history_list();
        if (hist_list) {
            for (int i = 0; hist_list[i] != NULL; i++) {
                printf("%d: %s\n", i + history_base, hist_list[i]->line);
            }
        }
        return 1;
    }

    if (strcmp(args[0], "export") == 0) {
        if (args[1] == NULL) {
            fprintf(stderr, "export: usage: export VAR=value\n");
        } else {
            char *var = strdup(args[1]);
            char *eq = strchr(var, '=');
            if (eq) {
                *eq = '\0';
                char *value = eq + 1;
                if (setenv(var, value, 1) != 0) {
                    perror("export");
                }
            } else {
                fprintf(stderr, "export: invalid format: %s\n", args[1]);
            }
            free(var);
        }
        return 1;
    }

    return 0; // 不是內建指令
}

int main() {
    char *input;
    char *command_str;
    int background;

    // 安裝 SIGINT 處理程序
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // 重新啟動被中斷的系統調用
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }


    while (1) {
        // 取得使用者名稱
        struct passwd *pw = getpwuid(getuid());
        if (!pw) {
            perror("getpwuid failed");
            exit(EXIT_FAILURE);
        }
        const char *username = pw->pw_name;

        // 取得目前工作目錄
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd error");
            continue;
        }

        // 取得使用者主目錄
        const char *home_dir = getenv("HOME");
        if (!home_dir) {
            pw = getpwuid(getuid());
            if (pw)
                home_dir = pw->pw_dir;
            else
                home_dir = "";
        }

        // 構建顯示路徑，若在主目錄或其子目錄，顯示 ~
        char display_path[PATH_MAX];
        if (strncmp(cwd, home_dir, strlen(home_dir)) == 0) {
            if (strlen(cwd) == strlen(home_dir)) {
                strcpy(display_path, "~");
            } else if (cwd[strlen(home_dir)] == '/') {
                snprintf(display_path, sizeof(display_path), "~%s", cwd + strlen(home_dir));
            } else {
                strcpy(display_path, cwd);
            }
        } else {
            strcpy(display_path, cwd);
        }

        // 構建提示符（可選：使用顏色）
        char prompt[PATH_MAX + 64];
        snprintf(prompt, sizeof(prompt), "%s:%s$ ", username, display_path);
        // 若要使用顏色，可以這樣：
        // snprintf(prompt, sizeof(prompt), COLOR_GREEN "%s" COLOR_RESET ":%s$ ", username, display_path);

        // 使用 readline 讀取輸入
        input = readline(prompt);

        // 判斷 EOF（如按下 Ctrl+D）
        if (input == NULL) {
            printf("\n");
            break;
        }

        // 去除輸入前後的空白
        char *trimmed_input = input;
        while (*trimmed_input == ' ' || *trimmed_input == '\t') trimmed_input++;
        // 去除末尾空白
        char *end = trimmed_input + strlen(trimmed_input) - 1;
        while (end > trimmed_input && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        // 處理歷史命令展開（如 !!）
        if (strstr(trimmed_input, "!!")) {
            char *expanded_input = expand_history(trimmed_input);
            if (!expanded_input) {
                free(input);
                continue;
            }
            free(input);
            input = expanded_input;
            trimmed_input = input;
            printf("%s\n", trimmed_input); // 顯示展開後的命令
        }

        // 判斷是否輸入 exit
        if (trimmed_input && strcmp(trimmed_input, "exit") == 0) {
            free(input);
            break;
        }

        // 如果輸入不為空，加入歷史記錄
        if (trimmed_input && *trimmed_input) {
            add_history(trimmed_input);
        }

        // 檢查是否有背景執行符號 '&'
        size_t input_len = strlen(trimmed_input);
        if (input_len > 0 && trimmed_input[input_len - 1] == '&') {
            background = 1;
            trimmed_input[input_len - 1] = '\0'; // 移除 '&'
            // 去除末尾空白
            end = trimmed_input + strlen(trimmed_input) - 1;
            while (end > trimmed_input && (*end == ' ' || *end == '\t')) {
                *end = '\0';
                end--;
            }
        } else {
            background = 0;
        }

        // 解析輸入為參數陣列，用於判斷是否為內建命令
        char *saveptr;
        char *token;
        char *args[MAX_ARGS];
        int arg_count = 0;

        char *input_copy = strdup(trimmed_input);
        if (!input_copy) {
            perror("strdup failed");
            free(input);
            continue;
        }

        token = strtok_r(input_copy, " \t", &saveptr);
        while (token != NULL && arg_count < MAX_ARGS - 1) {
            // 處理引號
            if (token[0] == '"' || token[0] == '\'') {
                char quote = token[0];
                token++;
                char *end_quote = strchr(token, quote);
                if (end_quote) {
                    *end_quote = '\0';
                }
                args[arg_count++] = strdup(token);
            } else {
                args[arg_count++] = strdup(token);
            }
            token = strtok_r(NULL, " \t", &saveptr);
        }
        args[arg_count] = NULL;

        // 進行 tilde 擴展
        for (int i = 0; i < arg_count; i++) {
            if (args[i][0] == '~') {
                char *expanded = tilde_expansion(args[i]);
                if (expanded) {
                    free(args[i]);
                    args[i] = expanded;
                }
            }
        }

        // 檢查是否為內建命令
        if (arg_count > 0 && is_built_in(args[0])) {
            handle_built_in(args);
        } else if (arg_count > 0) {
            // 外部命令，使用 sh -c 來執行
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork error");
                // 釋放記憶體
                for (int i = 0; i < arg_count; i++) {
                    free(args[i]);
                }
                free(input_copy);
                free(input);
                continue;
            } else if (pid == 0) {
                // 子進程

                // 恢復 SIGINT 的預設行為
                struct sigaction sa_child;
                sa_child.sa_handler = SIG_DFL;
                sigemptyset(&sa_child.sa_mask);
                sa_child.sa_flags = 0;
                if (sigaction(SIGINT, &sa_child, NULL) == -1) {
                    perror("sigaction in child");
                    exit(EXIT_FAILURE);
                }

                // 構建完整的命令字符串
                // 將 args 重新組合為一個命令字符串
                size_t cmd_length = 0;
                for (int i = 0; i < arg_count; i++) {
                    cmd_length += strlen(args[i]) + 1; // 空格或結尾
                }
                char *cmd_str = malloc(cmd_length + 1);
                if (!cmd_str) {
                    perror("malloc failed");
                    exit(EXIT_FAILURE);
                }
                cmd_str[0] = '\0';
                for (int i = 0; i < arg_count; i++) {
                    strcat(cmd_str, args[i]);
                    if (i < arg_count - 1) {
                        strcat(cmd_str, " ");
                    }
                }

                // 執行 sh -c "cmd_str"
                execlp("sh", "sh", "-c", cmd_str, (char *)NULL);

                // 如果 execlp 返回，表示執行失敗
                perror("execlp failed");
                free(cmd_str);
                exit(EXIT_FAILURE);
            } else {
                // 父進程
                if (!background) {
                    int status;
                    if (waitpid(pid, &status, 0) < 0) {
                        perror("waitpid error");
                    }
                } else {
                    printf("Started background process with PID %d\n", pid);
                }
            }
        }

        // 釋放記憶體
        for (int i = 0; i < arg_count; i++) {
            free(args[i]);
        }
        free(input_copy);
        free(input);
    }

    return 0;
}
