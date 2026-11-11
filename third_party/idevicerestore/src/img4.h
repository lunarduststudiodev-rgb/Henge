/*
 * img4.h
 * Functions for handling the IMG4 format
 *
 * Copyright (c) 2013-2019 Nikias Bassen. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef IDEVICERESTORE_IMG4_H
#define IDEVICERESTORE_IMG4_H

#ifdef __cplusplus
extern "C" {
#endif

struct img4_envelope {
	unsigned char prefix[16];
	unsigned int prefix_size;
	unsigned char *suffix;
	size_t suffix_size;
};

/* Build the bytes surrounding an IM4P payload. Only its opening fields need
 * to be in memory; the component type is patched there when required. */
int img4_build_envelope(const char* component_name, void* component_prefix, size_t prefix_size,
	size_t component_size, plist_t parameters, plist_t tss_response, struct img4_envelope* envelope);

int img4_stitch_component(const char* component_name, const void* component_data, size_t component_size, plist_t parameters, plist_t tss_response, void** img4_data, size_t *img4_size);
int img4_create_local_manifest(plist_t request, plist_t build_identity, plist_t* manifest);

#ifdef __cplusplus
}
#endif

#endif
