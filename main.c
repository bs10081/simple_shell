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

// 定義 ANSI 顏色代碼
#define COLOR_RESET   "\x1b[0m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_RED     "\x1b[31m"

// 定義最大參數數量
#define MAX_ARGS 128
#define MAX_COMMANDS 16

// 全局變數存儲動態命令列表
char **dynamic_commands = NULL;
size_t dynamic_commands_count = 0;

// 自定義補全函數的前向宣告
char **custom_completion(const char *text, int start, int end);
char *command_generator(const char *text, int state);

// 內建指令列表
const char *built_in_commands[] = {"cd", "exit", "echo", "help", "history", "export", NULL};

// 解析後的命令結構
typedef struct command {
    char *name;
    char **args;
    char *input_redirection;
    char *output_redirection;
    int append; // 1 表示 >>，0 表示 >
    struct command *next;
} command_t;

// 函數來動態獲取命令列表
void load_dynamic_commands() {
    char *path_env = getenv("PATH");
    if (!path_env) return;

    char *path = strdup(path_env);
    if (!path) {
        perror("strdup failed");
        exit(EXIT_FAILURE);
    }

    char *dir = strtok(path, ":");
    while (dir != NULL) {
        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                // 濾除 "." 和 ".."
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;

                // 檢查是否為可執行文件
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
                struct stat sb;
                if (stat(full_path, &sb) == 0 && (sb.st_mode & S_IXUSR)) {
                    // 添加到命令列表
                    dynamic_commands = realloc(dynamic_commands, sizeof(char*) * (dynamic_commands_count + 2));
                    if (!dynamic_commands) {
                        perror("realloc failed");
                        free(path);
                        exit(EXIT_FAILURE);
                    }
                    dynamic_commands[dynamic_commands_count++] = strdup(entry->d_name);
                    dynamic_commands[dynamic_commands_count] = NULL; // 結尾設為 NULL
                }
            }
            closedir(dp);
        }
        dir = strtok(NULL, ":");
    }
    free(path);
}

// Signal handler for SIGINT
void sigint_handler(int sig) {
    // 在接收到 SIGINT 時，清除當前輸入並顯示新的提示符
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

// 釋放命令結構的記憶體
void free_commands(command_t *head) {
    command_t *current = head;
    while (current) {
        command_t *next = current->next;
        free(current->name);
        for (int i = 0; current->args[i] != NULL; i++) {
            free(current->args[i]);
        }
        free(current->args);
        if (current->input_redirection)
            free(current->input_redirection);
        if (current->output_redirection)
            free(current->output_redirection);
        free(current);
        current = next;
    }
}

// 解析輸入指令行
command_t* parse_input(char *input, int *background) {
    command_t *head = NULL;
    command_t *current = NULL;

    char *token;
    char *saveptr;
    int cmd_index = 0;

    // 檢查是否有背景執行符號 '&'
    if (input[strlen(input) - 1] == '&') {
        *background = 1;
        input[strlen(input) - 1] = '\0'; // 移除 '&'
    } else {
        *background = 0;
    }

    // 使用 strtok_r 以 '|' 分隔多個命令
    token = strtok_r(input, "|", &saveptr);
    while (token != NULL && cmd_index < MAX_COMMANDS) {
        command_t *cmd = malloc(sizeof(command_t));
        if (!cmd) {
            perror("malloc failed");
            free_commands(head);
            return NULL;
        }
        cmd->next = NULL;
        cmd->input_redirection = NULL;
        cmd->output_redirection = NULL;
        cmd->append = 0;

        // 解析每個命令中的重定向
        char *redir_ptr = strchr(token, '>');
        if (redir_ptr) {
            if (*(redir_ptr + 1) == '>') {
                cmd->append = 1;
                *redir_ptr = '\0';
                redir_ptr += 2;
            } else {
                cmd->append = 0;
                *redir_ptr = '\0';
                redir_ptr += 1;
            }
            while (*redir_ptr == ' ') redir_ptr++; // 跳過空格
            char *filename = strtok(redir_ptr, " ");
            if (filename) {
                cmd->output_redirection = strdup(filename);
            }
        }

        redir_ptr = strchr(token, '<');
        if (redir_ptr) {
            *redir_ptr = '\0';
            redir_ptr += 1;
            while (*redir_ptr == ' ') redir_ptr++; // 跳過空格
            char *filename = strtok(redir_ptr, " ");
            if (filename) {
                cmd->input_redirection = strdup(filename);
            }
        }

        // 解析命令名稱和參數
        char **args = malloc(sizeof(char*) * (MAX_ARGS));
        if (!args) {
            perror("malloc failed");
            free(cmd);
            free_commands(head);
            return NULL;
        }
        int arg_count = 0;

        // 使用 strtok_r 以空格分隔
        char *arg_token = strtok_r(token, " \t", &saveptr);
        while (arg_token != NULL && arg_count < MAX_ARGS - 1) {
            // 處理引號
            if (arg_token[0] == '"' || arg_token[0] == '\'') {
                char quote = arg_token[0];
                arg_token++;
                char *end_quote = strchr(arg_token, quote);
                if (end_quote) {
                    *end_quote = '\0';
                }
                args[arg_count++] = strdup(arg_token);
            } else {
                args[arg_count++] = strdup(arg_token);
            }
            arg_token = strtok_r(NULL, " \t", &saveptr);
        }
        args[arg_count] = NULL;

        if (arg_count > 0) {
            cmd->name = strdup(args[0]);
            cmd->args = args;
        } else {
            cmd->name = NULL;
            free(args);
        }

        // 添加到命令鏈表
        if (!head) {
            head = cmd;
            current = cmd;
        } else {
            current->next = cmd;
            current = cmd;
        }

        cmd_index++;
        token = strtok_r(NULL, "|", &saveptr);
    }

    return head;
}

// 內建指令處理函數
int handle_built_in(command_t *cmd) {
    if (strcmp(cmd->name, "cd") == 0) {
        if (cmd->args[1] == NULL) {
            const char *home_dir = getenv("HOME");
            if (home_dir == NULL) {
                struct passwd *pw = getpwuid(getuid());
                home_dir = pw->pw_dir;
            }
            if (chdir(home_dir) != 0) {
                perror("cd");
            }
        } else {
            if (chdir(cmd->args[1]) != 0) {
                perror("cd");
            }
        }
        return 1;
    }

    if (strcmp(cmd->name, "exit") == 0) {
        exit(0);
    }

    if (strcmp(cmd->name, "echo") == 0) {
        for (int i = 1; cmd->args[i] != NULL; i++) {
            printf("%s ", cmd->args[i]);
        }
        printf("\n");
        return 1;
    }

    if (strcmp(cmd->name, "help") == 0) {
        printf("Simple Shell Built-in Commands:\n");
        printf("  cd [dir]      Change the current directory to 'dir'.\n");
        printf("  exit          Exit the shell.\n");
        printf("  echo [args]   Display the given arguments.\n");
        printf("  help          Display this help message.\n");
        printf("  history       Show command history.\n");
        printf("  export VAR=val Set environment variable VAR to val.\n");
        return 1;
    }

    if (strcmp(cmd->name, "history") == 0) {
        HIST_ENTRY **hist_list = history_list();
        if (hist_list) {
            for (int i = 0; hist_list[i] != NULL; i++) {
                printf("%d: %s\n", i + history_base, hist_list[i]->line);
            }
        }
        return 1;
    }

    if (strcmp(cmd->name, "export") == 0) {
        if (cmd->args[1] == NULL) {
            fprintf(stderr, "export: usage: export VAR=value\n");
        } else {
            char *var = strdup(cmd->args[1]);
            char *eq = strchr(var, '=');
            if (eq) {
                *eq = '\0';
                char *value = eq + 1;
                if (setenv(var, value, 1) != 0) {
                    perror("export");
                }
            } else {
                fprintf(stderr, "export: invalid format: %s\n", cmd->args[1]);
            }
            free(var);
        }
        return 1;
    }

    return 0; // 不是內建指令
}

// 執行單個命令（不處理管道）
int execute_command(command_t *cmd, int input_fd, int output_fd, int background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork error");
        return -1;
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

        // 處理輸入重定向
        if (cmd->input_redirection) {
            int fd = open(cmd->input_redirection, O_RDONLY);
            if (fd < 0) {
                perror("open input_redirection");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        } else if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }

        // 處理輸出重定向
        if (cmd->output_redirection) {
            int fd;
            if (cmd->append) {
                fd = open(cmd->output_redirection, O_WRONLY | O_CREAT | O_APPEND, 0644);
            } else {
                fd = open(cmd->output_redirection, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            }
            if (fd < 0) {
                perror("open output_redirection");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        } else if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }

        // 執行命令
        // 變數展開
        for (int i = 0; cmd->args[i] != NULL; i++) {
            if (cmd->args[i][0] == '$') {
                char *var_name = cmd->args[i] + 1;
                char *var_value = getenv(var_name);
                if (var_value) {
                    free(cmd->args[i]);
                    cmd->args[i] = strdup(var_value);
                }
            }
        }

        execvp(cmd->name, cmd->args);
        // 如果 execvp 返回，說明執行失敗
        perror("Command execution failed");
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
    return 0;
}

// 執行命令鏈表（處理管道）
int execute_commands(command_t *head, int background) {
    command_t *current = head;
    int input_fd = STDIN_FILENO;
    int pipe_fd[2];

    while (current) {
        if (current->next) {
            if (pipe(pipe_fd) < 0) {
                perror("pipe");
                return -1;
            }
        } else {
            pipe_fd[0] = STDIN_FILENO;
            pipe_fd[1] = STDOUT_FILENO;
        }

        // 處理內建指令
        if (handle_built_in(current)) {
            // 如果是內建指令且不是管道中的第一個指令
            if (current->next) {
                // 內建指令在管道中不支援
                fprintf(stderr, "Built-in commands cannot be used in pipelines.\n");
                return -1;
            }
            // 內建指令已處理
        } else {
            execute_command(current, input_fd, pipe_fd[1], background);
        }

        // 關閉不需要的文件描述符
        if (pipe_fd[1] != STDOUT_FILENO)
            close(pipe_fd[1]);
        if (input_fd != STDIN_FILENO)
            close(input_fd);

        input_fd = pipe_fd[0];
        current = current->next;
    }

    return 0;
}

// 自定義補全函數
char **custom_completion(const char *text, int start, int end) {
    // 當前補全在命令的第一個參數時，補全命令
    // 否則，使用默認的文件補全
    if (start == 0) {
        return rl_completion_matches(text, command_generator);
    } else {
        // 使用默認的文件補全
        return rl_completion_matches(text, rl_filename_completion_function);
    }
}

// 命令生成器，用於生成命令補全
char *command_generator(const char *text, int state) {
    static size_t list_index;
    const char *cmd;

    // 首次呼叫時初始化索引
    if (state == 0) {
        list_index = 0;
    }

    // 補全內建指令
    while (built_in_commands[list_index] != NULL) {
        cmd = built_in_commands[list_index];
        list_index++;
        if (strncmp(cmd, text, strlen(text)) == 0) {
            return strdup(cmd);
        }
    }

    // 補全動態命令
    while (list_index < dynamic_commands_count) {
        cmd = dynamic_commands[list_index++];
        if (strncmp(cmd, text, strlen(text)) == 0) {
            return strdup(cmd);
        }
    }

    return NULL;
}

int main() {
    char *input;
    command_t *cmd_head;
    int background;

    // 加載動態命令
    load_dynamic_commands();

    // 安裝 SIGINT 處理程序
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // 重新啟動被中斷的系統調用
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    // 初始化 Readline 補全
    rl_attempted_completion_function = custom_completion;

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

        // 構建提示符
        char prompt[PATH_MAX + 64];
        snprintf(prompt, sizeof(prompt), "%s:%s$ ", username, cwd);

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

        // 判斷是否輸入 exit
        if (trimmed_input && strcmp(trimmed_input, "exit") == 0) {
            free(input);
            break;
        }

        // 如果輸入不為空，加入歷史記錄
        if (trimmed_input && *trimmed_input) {
            add_history(trimmed_input);
        }

        // 解析輸入
        cmd_head = parse_input(trimmed_input, &background);
        if (!cmd_head) {
            free(input);
            continue;
        }

        // 執行命令
        execute_commands(cmd_head, background);

        // 釋放命令結構的記憶體
        free_commands(cmd_head);

        free(input);
    }

    // 釋放動態命令列表的記憶體
    for (size_t i = 0; i < dynamic_commands_count; i++) {
        free(dynamic_commands[i]);
    }
    free(dynamic_commands);

    return 0;
}
