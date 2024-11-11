#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/wait.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_ARGS 64

int main() {
    char *input;
    char *args[MAX_ARGS];
    char *token;
    pid_t pid;
    int status;

    while (1) {
        // 取得使用者名稱
        struct passwd *pw = getpwuid(getuid());
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
        char *trimmed_input = strtok(input, "\n");

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
        int arg_count = 0;
        token = strtok(trimmed_input, " ");
        while (token != NULL && arg_count < MAX_ARGS - 1) {
            args[arg_count++] = token;
            token = strtok(NULL, " ");
        }
        args[arg_count] = NULL;

        // 空輸入處理
        if (arg_count == 0) {
            free(input);
            continue;
        }

        // 創建子程序
        pid = fork();
        if (pid < 0) {
            perror("fork error");
            free(input);
            continue;
        } else if (pid == 0) {
            // 子程序，執行命令
            execvp(args[0], args);
            // 如果 execvp 返回，說明執行失敗
            perror("Command execution failed");
            exit(EXIT_FAILURE);
        } else {
            // 父程序，等待子程序結束
            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid error");
            }
        }

        free(input);
    }
    return 0;
}

