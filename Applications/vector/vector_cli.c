#include "vec_env.h"
#include "vec_eval.h"
#include "vec_cas.h"
#include "vec_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_left(const char **ps)
{
	const char *s = *ps;
	while (*s && isspace((unsigned char)*s))
		s++;
	*ps = s;
}

static int handle_command(vec_env *env, const char *line)
{
	trim_left(&line);
	if (!*line)
		return 0;

	if (!strcmp(line, "quit") || !strcmp(line, "q") || !strcmp(line, "exit"))
		return 1;

	if (!strcmp(line, "float")) {
		env->mode = VEC_MODE_FLOAT;
		return 0;
	}
	if (!strcmp(line, "exact")) {
		env->mode = VEC_MODE_EXACT;
		return 0;
	}
	if (!strncmp(line, "prec", 4) && isspace((unsigned char)line[4])) {
		line += 4;
		trim_left(&line);
		int v = atoi(line);
		if (v < 1)
			v = 1;
		if (v > 32)
			v = 32;
		env->prec = v;
		return 0;
	}
	return 0;
}

static void print_value(const vec_env *env, const vec_value *v)
{
	char buf[256];

	switch (v->kind) {
	case VEC_VALUE_NUMBER:
		(void)vec_number_string(v->num, env->prec, buf, sizeof(buf));
		printf("%s\n", buf);
		break;
	case VEC_VALUE_EXPR:
		if (v->expr) {
			(void)vec_node_to_string(v->expr, buf, sizeof(buf));
			printf("%s\n", buf);
		} else {
			printf("<expr>\n");
		}
		break;
	case VEC_VALUE_ARRAY:
		printf("[array len=%lu]\n", (unsigned long)v->len);
		break;
	case VEC_VALUE_MATRIX:
		printf("[matrix %dx%d]\n", v->rows, v->cols);
		break;
	case VEC_VALUE_COMPLEX:
	default:
		/* Keep it simple; UI build is for rich formatting. */
		printf("[value]\n");
		break;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	vec_env env;
	vec_env_init(&env);

	char line[256];
	for (;;) {
		fputs("V> ", stdout);
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin))
			break;

		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;

		const char *s = line;
		trim_left(&s);
		if (!*s)
			continue;

		if (*s == ':') {
			if (handle_command(&env, s + 1))
				break;
			continue;
		}

		char err[128];
		err[0] = 0;
		vec_actions acts;
		if (vec_parse_input(s, &acts, err, sizeof(err)) != 0) {
			fprintf(stderr, "%s\n", err[0] ? err : "parse error");
			continue;
		}

		for (size_t i = 0; i < acts.count; i++) {
			vec_action *a = &acts.items[i];
			if (a->kind == VEC_ACT_ASSIGN_FUNC) {
				if (vec_env_set_func(&env, a->func_name, a->func_param, a->expr) != 0) {
					fprintf(stderr, "eval: out of memory\n");
					continue;
				}
				a->expr = NULL; /* ownership transferred */
				printf("%s(%s) = <expr>\n", a->func_name, a->func_param);
				continue;
			}
			if (a->kind == VEC_ACT_ASSIGN_VAR) {
				vec_value v;
				err[0] = 0;
				if (vec_eval_node(&env, a->expr, &v, err, sizeof(err)) != 0) {
					fprintf(stderr, "%s\n", err[0] ? err : "eval error");
					continue;
				}
				if (vec_env_set_var(&env, a->var_name, v) != 0) {
					fprintf(stderr, "eval: out of memory\n");
					vec_value_destroy(&v);
					continue;
				}
				printf("%s = ", a->var_name);
				print_value(&env, &v);
				continue;
			}

			vec_value v;
			err[0] = 0;
			if (vec_eval_node(&env, a->expr, &v, err, sizeof(err)) != 0) {
				fprintf(stderr, "%s\n", err[0] ? err : "eval error");
				continue;
			}
			print_value(&env, &v);
			vec_value_destroy(&v);
		}
		vec_actions_destroy(&acts);
	}

	vec_env_destroy(&env);
	return 0;
}
