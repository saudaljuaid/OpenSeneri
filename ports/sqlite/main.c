/* SPDX-License-Identifier: GPL-3.0-only */

#include "sqlite3.h"

#include <phipia/runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct query_result {
    int rows;
    int valid;
};

static int collect_row(void *context, int columns, char **values, char **names)
{
    static const char *const expected_names[] = {"alpha", "beta", "gamma"};
    static const char *const expected_values[] = {"11", "22", "33"};
    struct query_result *result = context;
    const int row = result->rows;
    (void)names;
    if (columns != 2 || row >= 3 || values[0] == NULL || values[1] == NULL ||
        strcmp(values[0], expected_names[row]) != 0 ||
        strcmp(values[1], expected_values[row]) != 0) {
        result->valid = 0;
        return 1;
    }
    ++result->rows;
    return 0;
}

static int integrity_row(void *context, int columns, char **values, char **names)
{
    int *valid = context;
    (void)names;
    *valid = columns == 1 && values[0] != NULL && strcmp(values[0], "ok") == 0;
    return *valid ? 0 : 1;
}

static int execute(sqlite3 *database, const char *sql)
{
    char *message = NULL;
    const int status = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (status != SQLITE_OK) {
        fprintf(stderr, "SQLite error %d: %s\n", status,
            message == NULL ? sqlite3_errmsg(database) : message);
    }
    sqlite3_free(message);
    return status;
}

static int first_phase(void)
{
    sqlite3 *primary = NULL;
    sqlite3 *contender = NULL;
    char *message = NULL;
    uint64_t transaction_started;
    uint64_t transaction_elapsed;
    int status;

    if (sqlite3_open_v2("PORT.DB", &primary,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK ||
        execute(primary, "PRAGMA journal_mode=DELETE;") != SQLITE_OK ||
        execute(primary, "PRAGMA synchronous=FULL;") != SQLITE_OK ||
        execute(primary, "CREATE TABLE rows(id INTEGER PRIMARY KEY, name TEXT NOT NULL, value INTEGER NOT NULL);") != SQLITE_OK) {
        goto failure;
    }
    transaction_started = phipia_monotonic_ns();
    status = execute(primary, "BEGIN IMMEDIATE; INSERT INTO rows(name,value) VALUES('alpha',11),('beta',22),('gamma',33); COMMIT;");
    transaction_elapsed = phipia_monotonic_ns() - transaction_started;
    if (status != SQLITE_OK ||
        sqlite3_open_v2("PORT.DB", &contender, SQLITE_OPEN_READWRITE, NULL) !=
            SQLITE_OK || execute(primary, "BEGIN IMMEDIATE;") != SQLITE_OK) {
        goto failure;
    }
    status = sqlite3_exec(contender, "BEGIN IMMEDIATE;", NULL, NULL, &message);
    sqlite3_free(message);
    message = NULL;
    if (status != SQLITE_BUSY || execute(primary, "ROLLBACK;") != SQLITE_OK ||
        sqlite3_close(contender) != SQLITE_OK || sqlite3_close(primary) != SQLITE_OK) {
        contender = NULL;
        primary = NULL;
        goto failure;
    }
    printf("PHIPIA PERF sqlite transaction_ns=%llu\n",
        (unsigned long long)transaction_elapsed);
    puts("PHIPIA SQLITE PHASE1 PASS rows=3 locking=busy");
    return 0;

failure:
    sqlite3_free(message);
    if (contender != NULL) (void)sqlite3_close(contender);
    if (primary != NULL) (void)sqlite3_close(primary);
    return 1;
}

static int second_phase(void)
{
    static const char output[] = "rows=3\nsum=66\nintegrity=ok\n";
    struct query_result query = {0, 1};
    sqlite3 *database = NULL;
    char *message = NULL;
    int integrity = 0;
    uint64_t reopen_started;
    uint64_t reopen_elapsed;
    FILE *result;
    int write_ok;

    reopen_started = phipia_monotonic_ns();
    if (sqlite3_open_v2("PORT.DB", &database, SQLITE_OPEN_READWRITE, NULL) !=
            SQLITE_OK || sqlite3_exec(database,
            "SELECT name, value FROM rows ORDER BY id;", collect_row, &query,
            &message) != SQLITE_OK || !query.valid || query.rows != 3) {
        goto failure;
    }
    sqlite3_free(message);
    message = NULL;
    if (sqlite3_exec(database, "PRAGMA integrity_check;", integrity_row,
            &integrity, &message) != SQLITE_OK || !integrity ||
        sqlite3_close(database) != SQLITE_OK) {
        database = NULL;
        goto failure;
    }
    database = NULL;
    reopen_elapsed = phipia_monotonic_ns() - reopen_started;
    result = fopen("RESULT.TXT", "w");
    if (result == NULL) return 1;
    write_ok = fwrite(output, 1U, sizeof(output) - 1U, result) ==
        sizeof(output) - 1U;
    if (fclose(result) != 0 || !write_ok) return 1;
    printf("PHIPIA PERF sqlite reopen_query_ns=%llu\n",
        (unsigned long long)reopen_elapsed);
    puts("PHIPIA SQLITE PHASE2 PASS rows=3 sum=66 integrity=ok");
    return 0;

failure:
    if (message != NULL) fprintf(stderr, "SQLite query error: %s\n", message);
    sqlite3_free(message);
    if (database != NULL) (void)sqlite3_close(database);
    return 1;
}

int main(void)
{
    struct stat database;
    if (stat("PORT.DB", &database) == 0) return second_phase();
    if (errno != ENOENT) return 1;
    return first_phase();
}
