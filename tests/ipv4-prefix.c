/*
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <talloc.h>

#include "../src/ip-util.h"
#include "../src/ip-util.c"

int main()
{
	char *p;

	p = ipv4_prefix_to_strmask(NULL, 32);
	if (p == NULL || strcmp(p, "255.255.255.255") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 30);
	if (p == NULL || strcmp(p, "255.255.255.252") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 27);
	if (p == NULL || strcmp(p, "255.255.255.224") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 24);
	if (p == NULL || strcmp(p, "255.255.255.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 22);
	if (p == NULL || strcmp(p, "255.255.252.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 20);
	if (p == NULL || strcmp(p, "255.255.240.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 18);
	if (p == NULL || strcmp(p, "255.255.192.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 16);
	if (p == NULL || strcmp(p, "255.255.0.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 8);
	if (p == NULL || strcmp(p, "255.0.0.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 5);
	if (p == NULL || strcmp(p, "248.0.0.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_prefix_to_strmask(NULL, 3);
	if (p == NULL || strcmp(p, "224.0.0.0") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	/* Check ipv4_route_to_cidr */
	p = ipv4_route_to_cidr(NULL, "192.168.5.0/255.255.255.0");
	if (p == NULL || strcmp(p, "192.168.5.0/24") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	p = ipv4_route_to_cidr(NULL, "192.168.4.0/255.255.0.0");
	if (p == NULL || strcmp(p, "192.168.4.0/16") != 0) {
		fprintf(stderr, "error in %d: %s\n", __LINE__, p);
		exit(1);
	}
	talloc_free(p);

	return 0;
}
