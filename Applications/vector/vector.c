#include "vec_env.h"
#include "vec_eval.h"
#include "vec_parser.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
	fprintf(out, "vector (FUZIX) - Spark Vector port (WIP)\n");
	fprintf(out, "usage: vector\n");
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (argc > 1) {
		usage(stderr);
		return 1;
	}

	vec_env env;
	vec_env_init(&env);

	puts("Vector (FUZIX) - port in progress");
	puts("Type :help for commands. :quit to exit.");

	for (;;) {
		char line[256];
		char err[128];
		fputs("V> ", stdout);
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin)) {
			if (ferror(stdin))
				fprintf(stderr, "read: %s\n", strerror(errno));
			break;
		}
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (!strcmp(line, ":quit") || !strcmp(line, ":q"))
			break;
		if (!line[0])
			continue;

		if (line[0] == ':') {
			if (!strcmp(line, ":help")) {
				puts(":help        show help");
				puts(":quit/:q     exit");
				puts(":exact       exact (rational) mode");
				puts(":float       float mode");
				puts(":prec N      set print precision (1..32)");
				continue;
			}
			if (!strcmp(line, ":exact")) {
				env.mode = VEC_MODE_EXACT;
				puts("mode: exact");
				continue;
			}
			if (!strcmp(line, ":float")) {
				env.mode = VEC_MODE_FLOAT;
				puts("mode: float");
				continue;
			}
			if (!strncmp(line, ":prec", 5)) {
				char *p = line + 5;
				while (*p == ' ')
					p++;
				int v = atoi(p);
				if (v < 1 || v > 32) {
					puts("prec: 1..32");
					continue;
				}
				env.prec = v;
				printf("prec: %d\n", v);
				continue;
			}
			puts("unknown command");
			continue;
		}

		memset(err, 0, sizeof(err));
		vec_actions acts;
		if (vec_parse_input(line, &acts, err, sizeof(err)) != 0) {
			fprintf(stderr, "%s\n", err[0] ? err : "parse error");
			continue;
		}

		for (size_t i = 0; i < acts.count; i++) {
			vec_action *a = &acts.items[i];
			if (a->kind == VEC_ACT_ASSIGN_VAR) {
				vec_value v;
				memset(err, 0, sizeof(err));
				if (vec_eval_node(&env, a->expr, &v, err, sizeof(err)) != 0) {
					fprintf(stderr, "%s\n", err[0] ? err : "eval error");
					continue;
				}
				if (vec_env_set_var(&env, a->var_name, v) != 0) {
					fprintf(stderr, "eval: out of memory\n");
					vec_value_destroy(&v);
					continue;
				}
				{
					char buf[64];
					printf("%s = %s\n", a->var_name, vec_number_string(v.num, env.prec, buf, sizeof(buf)));
				}
				continue;
			}
			if (a->kind == VEC_ACT_ASSIGN_FUNC) {
				if (vec_env_set_func(&env, a->func_name, a->func_param, a->expr) != 0) {
					fprintf(stderr, "eval: out of memory\n");
					continue;
				}
				a->expr = NULL; /* ownership transferred */
				printf("%s(%s) = <expr>\n", a->func_name, a->func_param);
				continue;
			}
			{
				vec_value v;
				memset(err, 0, sizeof(err));
				if (vec_eval_node(&env, a->expr, &v, err, sizeof(err)) != 0) {
					fprintf(stderr, "%s\n", err[0] ? err : "eval error");
					continue;
				}
				if (v.kind == VEC_VALUE_NUMBER) {
					char buf[64];
					printf("%s\n", vec_number_string(v.num, env.prec, buf, sizeof(buf)));
				} else {
					puts("<value>");
				}
				vec_value_destroy(&v);
			}
		}

		vec_actions_destroy(&acts);
	}

	vec_env_destroy(&env);
	return 0;
}
