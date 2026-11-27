#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "proclaunch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

static const char EXPECTED_ARG_COMPLEX[] = "hello \"big\" world\\";
static const char EXPECTED_ARG_PATH[]    = "C:\\my dir\\";
static const char EXPECTED_ARG_EMPTY[]   = "";

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
#endif
}

static int test_background_and_poll(const char *self) {
    const char *argv[] = { self, "--child", "42", "1000", NULL };
    pl_process proc;

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 1 (фоновый запуск и pl_poll): pl_start вернула ошибку\n");
        return 0;
    }

    int iterations = 0;
    int exit_code = -1;
    int poll_res = 0;

    while ((poll_res = pl_poll(&proc, &exit_code)) == 0) {
        ++iterations;
        sleep_ms(100);
        if (iterations >= 100) {
            break;
        }
    }

    if (poll_res == 0) {
        pl_wait(&proc, &exit_code);
        printf("[FAIL] Тест 1 (фоновый запуск и pl_poll): превышено время ожидания\n");
        return 0;
    }

    if (poll_res == 1 && iterations > 3 && exit_code == 42) {
        printf("[ OK ] Тест 1 (фоновый запуск и pl_poll): итераций родителя = %d, код = %d\n",
               iterations, exit_code);
        return 1;
    }

    printf("[FAIL] Тест 1 (фоновый запуск и pl_poll): poll_res = %d, итераций = %d, код = %d (ожидалось >3 итераций и код 42)\n",
           poll_res, iterations, exit_code);
    return 0;
}

static int test_wait_exit_code(const char *self) {
    const char *argv[] = { self, "--child", "7", "300", NULL };
    pl_process proc;
    int exit_code = -1;

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 2 (pl_wait): pl_start вернула ошибку\n");
        return 0;
    }

    if (pl_wait(&proc, &exit_code) != 0) {
        printf("[FAIL] Тест 2 (pl_wait): pl_wait вернула ошибку\n");
        return 0;
    }

    if (exit_code == 7) {
        printf("[ OK ] Тест 2 (pl_wait): код возврата = %d\n", exit_code);
        return 1;
    }

    printf("[FAIL] Тест 2 (pl_wait): код возврата = %d (ожидалось 7)\n", exit_code);
    return 0;
}

static int test_arguments_quoting(const char *self) {
    const char *argv[] = {
        self,
        "--check-args",
        EXPECTED_ARG_COMPLEX,
        EXPECTED_ARG_PATH,
        EXPECTED_ARG_EMPTY,
        NULL
    };
    pl_process proc;
    int exit_code = -1;

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 3 (аргументы с пробелами и кавычками): pl_start вернула ошибку\n");
        return 0;
    }

    if (pl_wait(&proc, &exit_code) != 0) {
        printf("[FAIL] Тест 3 (аргументы с пробелами и кавычками): pl_wait вернула ошибку\n");
        return 0;
    }

    if (exit_code == 0) {
        printf("[ OK ] Тест 3 (аргументы с пробелами и кавычками): все аргументы получены без искажений\n");
        return 1;
    }

    printf("[FAIL] Тест 3 (аргументы с пробелами и кавычками): дочерний процесс обнаружил несоответствие (код = %d)\n",
           exit_code);
    return 0;
}

static int test_nonexistent_program(void) {
    const char *argv[] = { "no_such_program_12345", NULL };
    pl_process proc;

    int res = pl_start(argv, &proc);
    if (res == -1) {
        printf("[ OK ] Тест 4 (несуществующая программа): pl_start вернула -1\n");
        return 1;
    }

    pl_wait(&proc, NULL);
    printf("[FAIL] Тест 4 (несуществующая программа): pl_start вернула %d (ожидалось -1)\n", res);
    return 0;
}

static int test_exit_code_300(const char *self) {
    const char *argv[] = { self, "--child", "300", "100", NULL };
    pl_process proc;
    int exit_code = -1;

#ifdef _WIN32
    const int expected = 300;
#else
    const int expected = 44; /* 300 & 0xFF */
#endif

    if (pl_start(argv, &proc) != 0) {
        printf("[FAIL] Тест 5 (код возврата 300): pl_start вернула ошибку\n");
        return 0;
    }

    if (pl_wait(&proc, &exit_code) != 0) {
        printf("[FAIL] Тест 5 (код возврата 300): pl_wait вернула ошибку\n");
        return 0;
    }

    if (exit_code == expected) {
        printf("[ OK ] Тест 5 (код возврата 300): получен код = %d (ожидалось %d)\n",
               exit_code, expected);
        return 1;
    }

    printf("[FAIL] Тест 5 (код возврата 300): получен код = %d (ожидалось %d)\n",
           exit_code, expected);
    return 0;
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    /* Режим дочернего процесса для тестов */
    if (argc >= 4 && strcmp(argv[1], "--child") == 0) {
        sleep_ms(atoi(argv[3]));
        return atoi(argv[2]);
    }

    if (argc >= 2 && strcmp(argv[1], "--check-args") == 0) {
        if (argc != 5) {
            return 2;
        }
        if (strcmp(argv[2], EXPECTED_ARG_COMPLEX) != 0) {
            return 3;
        }
        if (strcmp(argv[3], EXPECTED_ARG_PATH) != 0) {
            return 4;
        }
        if (strcmp(argv[4], EXPECTED_ARG_EMPTY) != 0) {
            return 5;
        }
        return 0;
    }

    /* Удобный режим для демонстрации произвольного кода возврата */
    if (argc >= 3 && strcmp(argv[1], "--demo-code") == 0) {
        const char *delay = (argc >= 4) ? argv[3] : "200";
        const char *child_argv[] = { argv[0], "--child", argv[2], delay, NULL };
        pl_process proc;
        int exit_code = -1;

        if (pl_start(child_argv, &proc) != 0) {
            printf("[FAIL] Не удалось запустить дочерний процесс\n");
            return 1;
        }
        printf("Запущен дочерний процесс (PID = %ld), ожидание...\n", proc.pid);
        if (pl_wait(&proc, &exit_code) != 0) {
            printf("[FAIL] Ошибка ожидания процесса\n");
            return 1;
        }
        printf("Дочерний процесс завершился с кодом %d\n", exit_code);
        return 0;
    }

    /* Удобный режим для запуска произвольной внешней программы */
    if (argc >= 3 && strcmp(argv[1], "--run") == 0) {
        pl_process proc;
        int exit_code = -1;

        if (pl_start((const char *const *)(argv + 2), &proc) != 0) {
            printf("[FAIL] Не удалось запустить программу: %s\n", argv[2]);
            return 1;
        }
        printf("Запущена программа %s (PID = %ld)\n", argv[2], proc.pid);
        if (pl_wait(&proc, &exit_code) != 0) {
            printf("[FAIL] Ошибка ожидания процесса\n");
            return 1;
        }
        printf("Программа завершилась с кодом %d\n", exit_code);
        return 0;
    }

    const char *self = argv[0];
    int passed = 0;
    const int total = 5;

    passed += test_background_and_poll(self);
    passed += test_wait_exit_code(self);
    passed += test_arguments_quoting(self);
    passed += test_nonexistent_program();
    passed += test_exit_code_300(self);

    printf("\nИтого: %d/%d тестов пройдено\n", passed, total);
    return (passed == total) ? 0 : 1;
}
