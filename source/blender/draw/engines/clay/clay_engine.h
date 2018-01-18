/*
 * Copyright 2016, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Blender Institute
 *
 */

/** \file clay_engine.h
 *  \ingroup draw_engine
 */

#ifndef __CLAY_ENGINE_H__
#define __CLAY_ENGINE_H__

extern DrawEngineType draw_engine_clay_type;
extern struct RenderEngineType DRW_engine_viewport_clay_type;

struct IDProperty;

struct IDProperty *CLAY_render_settings_create(void);

/* Game engine transition */ // before structs were in clay_engine.c

/* declare members as empty (unused) */
typedef char DRWViewportEmptyList;

#define MAX_CLAY_MAT 512 /* 512 = 9 bit material id */

/* *********** LISTS *********** */

/**
* UBOs data needs to be 16 byte aligned (size of vec4)
*
* Reminder: float, int, bool are 4 bytes
*
* \note struct is expected to be initialized with all pad-bits zero'd
* so we can use 'memcmp' to check for duplicates. Possibly hash data later.
*/
typedef struct CLAY_UBO_Material {
	float ssao_params_var[4];
	/* - 16 -*/
	float matcap_hsv[3];
	float matcap_id; /* even float encoding have enough precision */
	/* - 16 -*/
	float matcap_rot[2];
	float pad[2]; /* ensure 16 bytes alignement */
} CLAY_UBO_Material; /* 48 bytes */
BLI_STATIC_ASSERT_ALIGN(CLAY_UBO_Material, 16)

typedef struct CLAY_HAIR_UBO_Material {
	float hair_randomness;
	float matcap_id;
	float matcap_rot[2];
	float matcap_hsv[3];
	float pad;
} CLAY_HAIR_UBO_Material; /* 32 bytes */
BLI_STATIC_ASSERT_ALIGN(CLAY_HAIR_UBO_Material, 16)

typedef struct CLAY_UBO_Storage {
	CLAY_UBO_Material materials[MAX_CLAY_MAT];
} CLAY_UBO_Storage;

typedef struct CLAY_HAIR_UBO_Storage {
	CLAY_HAIR_UBO_Material materials[MAX_CLAY_MAT];
} CLAY_HAIR_UBO_Storage;

/* GPUViewport.storage
* Is freed everytime the viewport engine changes */
typedef struct CLAY_Storage {
	/* Materials Parameter UBO */
	CLAY_UBO_Storage mat_storage;
	CLAY_HAIR_UBO_Storage hair_mat_storage;
	int ubo_current_id;
	int hair_ubo_current_id;
	DRWShadingGroup *shgrps[MAX_CLAY_MAT];
	DRWShadingGroup *shgrps_flat[MAX_CLAY_MAT];
	DRWShadingGroup *hair_shgrps[MAX_CLAY_MAT];
} CLAY_Storage;

typedef struct CLAY_StorageList {
	struct CLAY_Storage *storage;
	struct GPUUniformBuffer *mat_ubo;
	struct GPUUniformBuffer *hair_mat_ubo;
	struct CLAY_PrivateData *g_data;
} CLAY_StorageList;

typedef struct CLAY_FramebufferList {
	/* default */
	struct GPUFrameBuffer *default_fb;
	/* engine specific */
	struct GPUFrameBuffer *dupli_depth;
} CLAY_FramebufferList;

typedef struct CLAY_PassList {
	struct DRWPass *depth_pass;
	struct DRWPass *depth_pass_cull;
	struct DRWPass *clay_pass;
	struct DRWPass *clay_pass_flat;
	struct DRWPass *hair_pass;
} CLAY_PassList;

typedef struct CLAY_Data {
	void *engine_type;
	CLAY_FramebufferList *fbl;
	DRWViewportEmptyList *txl;
	CLAY_PassList *psl;
	CLAY_StorageList *stl;
} CLAY_Data;

typedef struct CLAY_ViewLayerData {
	struct GPUTexture *jitter_tx;
	struct GPUUniformBuffer *sampling_ubo;
	int cached_sample_num;
} CLAY_ViewLayerData;


/* *********** STATIC *********** */

typedef struct CLAY_StaticData {
	/* Depth Pre Pass */
	struct GPUShader *depth_sh;
	/* Shading Pass */
	struct GPUShader *clay_sh;
	struct GPUShader *clay_flat_sh;
	struct GPUShader *hair_sh;

	/* Matcap textures */
	struct GPUTexture *matcap_array;
	float matcap_colors[24][3];

	/* Ssao */
	float winmat[4][4];
	float viewvecs[3][4];
	float ssao_params[4];

	/* Just a serie of int from 0 to MAX_CLAY_MAT-1 */
	int ubo_mat_idxs[MAX_CLAY_MAT];

	/* engine specific */
	struct GPUTexture *depth_dup;
} CLAY_StaticData;


CLAY_Data *CLAY_engine_data_get();
CLAY_StaticData *CLAY_static_data_get();

#endif /* __CLAY_ENGINE_H__ */
