/*
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/KX_BlenderMaterial.cpp
 *  \ingroup ketsji
 */

#include "KX_BlenderMaterial.h"
#include "KX_Scene.h"
#include "KX_PyMath.h"

#include "BL_Shader.h"

#include "EXP_ListWrapper.h"

#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"

#include "GPU_draw.h"
#include "GPU_material.h" // for GPU_BLEND_SOLID

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

extern "C" {
#  include "eevee_private.h"
#  include "DRW_render.h"
#  include "../gpu/intern/gpu_codegen.h"
}

KX_BlenderMaterial::KX_BlenderMaterial(
		RAS_Rasterizer *rasty,
		KX_Scene *scene,
		Material *mat,
		const std::string& name,
		GameSettings *game,
		int lightlayer)
	:RAS_IPolyMaterial(name, game),
	m_material(mat),
	m_customShader(nullptr),
	m_rasterizer(rasty),
	m_scene(scene),
	m_userDefBlend(false),
	m_constructed(false),
	m_lightLayer(lightlayer)
{
	m_alphablend = mat->blend_method;

	if (m_material->use_nodes && m_material->nodetree) {
		EEVEE_Data *vedata = EEVEE_engine_data_get();
		EEVEE_ViewLayerData *sldata = EEVEE_view_layer_data_get();
		EEVEE_LampsInfo *linfo = sldata->lamps;
		EEVEE_StorageList *stl = EEVEE_engine_data_get()->stl;
		const bool use_refract = ((m_material->blend_flag & MA_BL_SS_REFRACTION) != 0) && ((stl->effects->enabled_effects & EFFECT_REFRACT) != 0);
		const bool use_sss = ((m_material->blend_flag & MA_BL_SS_SUBSURFACE) != 0) && ((stl->effects->enabled_effects & EFFECT_SSS) != 0);
		const bool use_blend = (m_material->blend_method & MA_BM_BLEND) != 0;
		const bool use_translucency = ((m_material->blend_flag & MA_BL_TRANSLUCENCY) != 0) && ((stl->effects->enabled_effects & EFFECT_SSS) != 0);
		m_gpuMat = EEVEE_material_mesh_get(scene->GetBlenderScene(), m_material, vedata,
			use_blend, (m_material->blend_method == MA_BM_MULTIPLY), use_refract, use_sss, use_translucency, linfo->shadow_method);
	}
	else {
		m_gpuMat = nullptr;
	}

	m_zoffset = mat->zoffs;

	m_rasMode |= (mat->game.flag & GEMAT_BACKCULL) ? 0 : RAS_TWOSIDED;
	m_rasMode |= (mat->material_type == MA_TYPE_WIRE) ? RAS_WIRE : 0;

	// always zsort alpha + add
	if (ELEM(m_alphablend, MA_BM_ADD, MA_BM_MULTIPLY, MA_BM_BLEND)) {
		m_rasMode |= RAS_ALPHA;
		//m_rasMode |= (mat && (mat->game.alpha_blend & GEMAT_ALPHA_SORT)) ? RAS_ZSORT : 0;
	}

	// RAS_IPolyMaterial variables...
	m_flag |= ((mat->mode & MA_SHLESS) != 0) ? 0 : RAS_MULTILIGHT;
	m_flag |= RAS_BLENDERGLSL;
	m_flag |= ((mat->mode2 & MA_CASTSHADOW) != 0) ? RAS_CASTSHADOW : 0;
	//m_flag |= ((mat->mode & MA_ONLYCAST) != 0) ? RAS_ONLYSHADOW : 0;
}

KX_BlenderMaterial::~KX_BlenderMaterial()
{
	// cleanup work
	if (m_constructed) {
		// clean only if material was actually used
		OnExit();
	}
}

BL_Shader *KX_BlenderMaterial::GetCustomShader() const
{
	if (m_customShader && m_customShader->Ok()) {
		return m_customShader;
	}

	return nullptr;
}

const std::string KX_BlenderMaterial::GetTextureName() const
{
	return (m_textures[0] ? m_textures[0]->GetName() : "");
}

Material *KX_BlenderMaterial::GetBlenderMaterial() const
{
	return m_material;
}

Scene *KX_BlenderMaterial::GetBlenderScene() const
{
	return m_scene->GetBlenderScene();
}

SCA_IScene *KX_BlenderMaterial::GetScene() const
{
	return m_scene;
}

void KX_BlenderMaterial::InitTextures()
{
	if (!m_gpuMat) {
		return;
	}
	GPUPass *gpupass = GPU_material_get_pass(m_gpuMat);

	if (!gpupass) {
		/* Shader compilation error */
		return;
	}

	/* Converting dynamic GPUInput to DRWUniform */
	ListBase *inputs = &gpupass->inputs;

	int i = 0;
	for (GPUInput *input = (GPUInput *)inputs->first; input; input = input->next) {
		/* Textures */
		if (input->ima) {
			BL_Texture *texture = new BL_Texture(input);
			m_textures[i] = texture;
			i++;
		}
	}
}

void KX_BlenderMaterial::OnConstruction()
{
	if (m_constructed) {
		// when material are reused between objects
		return;
	}

	InitTextures();
	//m_scene->GetBucketManager()->UpdateShaders(this);

	m_blendFunc[0] = 0;
	m_blendFunc[1] = 0;
	m_constructed = true;
}

void KX_BlenderMaterial::EndFrame(RAS_Rasterizer *rasty)
{
	rasty->SetAlphaBlend(GPU_BLEND_SOLID);
	RAS_Texture::DesactiveTextures();
}

void KX_BlenderMaterial::OnExit()
{
	if (m_customShader) {
		delete m_customShader;
	}
}

void KX_BlenderMaterial::UpdateIPO(
    MT_Vector4 rgba,
    MT_Vector3 specrgb,
    MT_Scalar hard,
    MT_Scalar spec,
    MT_Scalar ref,
    MT_Scalar emit,
	MT_Scalar ambient,
    MT_Scalar alpha,
	MT_Scalar specalpha)
{
	// only works one deep now

	// GLSL								Input
	m_material->specr = (float)(specrgb)[0];
	m_material->specg = (float)(specrgb)[1];
	m_material->specb = (float)(specrgb)[2];
	m_material->r = (float)(rgba[0]);
	m_material->g = (float)(rgba[1]);
	m_material->b = (float)(rgba[2]);
	m_material->alpha = (float)(rgba[3]);
	m_material->amb = (float)(ambient);
	m_material->har = (float)(hard);
	m_material->emit = (float)(emit);
	m_material->spec = (float)(spec);
	m_material->ref = (float)(ref);
	m_material->spectra = (float)specalpha;
}

void KX_BlenderMaterial::ReplaceScene(KX_Scene *scene)
{
	OnConstruction();
}

std::string KX_BlenderMaterial::GetName()
{
	return m_name;
}

#ifdef USE_MATHUTILS

#define MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR 1
#define MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR 2

static unsigned char mathutils_kxblendermaterial_color_cb_index = -1; /* index for our callbacks */

static int mathutils_kxblendermaterial_generic_check(BaseMathObject *bmo)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

	return 0;
}

static int mathutils_kxblendermaterial_color_get(BaseMathObject *bmo, int subtype)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

	Material *mat = self->GetBlenderMaterial();

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			bmo->data[0] = mat->r;
			bmo->data[1] = mat->g;
			bmo->data[2] = mat->b;
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			bmo->data[0] = mat->specr;
			bmo->data[1] = mat->specg;
			bmo->data[2] = mat->specb;
			break;
		}
	}

	return 0;
}

static int mathutils_kxblendermaterial_color_set(BaseMathObject *bmo, int subtype)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(bmo->cb_user);
	if (!self)
		return -1;

	Material *mat = self->GetBlenderMaterial();

	switch (subtype) {
		case MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR:
		{
			mat->r = bmo->data[0];
			mat->g = bmo->data[1];
			mat->b = bmo->data[2];
			break;
		}
		case MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR:
		{
			mat->specr = bmo->data[0];
			mat->specg = bmo->data[1];
			mat->specb = bmo->data[2];
			break;
		}
	}

	return 0;
}

static int mathutils_kxblendermaterial_color_get_index(BaseMathObject *bmo, int subtype, int index)
{
	/* lazy, avoid repeteing the case statement */
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1)
		return -1;
	return 0;
}

static int mathutils_kxblendermaterial_color_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	/* lazy, avoid repeateing the case statement */
	if (mathutils_kxblendermaterial_color_get(bmo, subtype) == -1)
		return -1;

	bmo->data[index] = f;
	return mathutils_kxblendermaterial_color_set(bmo, subtype);
}

static Mathutils_Callback mathutils_kxblendermaterial_color_cb = {
	mathutils_kxblendermaterial_generic_check,
	mathutils_kxblendermaterial_color_get,
	mathutils_kxblendermaterial_color_set,
	mathutils_kxblendermaterial_color_get_index,
	mathutils_kxblendermaterial_color_set_index
};


void KX_BlenderMaterial_Mathutils_Callback_Init()
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_kxblendermaterial_color_cb_index = Mathutils_RegisterCallback(&mathutils_kxblendermaterial_color_cb);
}

#endif // USE_MATHUTILS

#ifdef WITH_PYTHON

PyMethodDef KX_BlenderMaterial::Methods[] =
{
	KX_PYMETHODTABLE(KX_BlenderMaterial, getShader),
	KX_PYMETHODTABLE( KX_BlenderMaterial, getTextureBindcode),
	KX_PYMETHODTABLE(KX_BlenderMaterial, setBlending),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef KX_BlenderMaterial::Attributes[] = {
	KX_PYATTRIBUTE_RO_FUNCTION("shader", KX_BlenderMaterial, pyattr_get_shader),
	KX_PYATTRIBUTE_RO_FUNCTION("textures", KX_BlenderMaterial, pyattr_get_textures),
	KX_PYATTRIBUTE_RW_FUNCTION("alpha", KX_BlenderMaterial, pyattr_get_alpha, pyattr_set_alpha),
	KX_PYATTRIBUTE_RW_FUNCTION("hardness", KX_BlenderMaterial, pyattr_get_hardness, pyattr_set_hardness),
	KX_PYATTRIBUTE_RW_FUNCTION("specularIntensity", KX_BlenderMaterial, pyattr_get_specular_intensity, pyattr_set_specular_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("specularColor", KX_BlenderMaterial, pyattr_get_specular_color, pyattr_set_specular_color),
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseIntensity", KX_BlenderMaterial, pyattr_get_diffuse_intensity, pyattr_set_diffuse_intensity),
	KX_PYATTRIBUTE_RW_FUNCTION("diffuseColor", KX_BlenderMaterial, pyattr_get_diffuse_color, pyattr_set_diffuse_color),
	KX_PYATTRIBUTE_RW_FUNCTION("emit", KX_BlenderMaterial, pyattr_get_emit, pyattr_set_emit),
	KX_PYATTRIBUTE_RW_FUNCTION("ambient", KX_BlenderMaterial, pyattr_get_ambient, pyattr_set_ambient),
	KX_PYATTRIBUTE_RW_FUNCTION("specularAlpha", KX_BlenderMaterial, pyattr_get_specular_alpha, pyattr_set_specular_alpha),

	KX_PYATTRIBUTE_NULL //Sentinel
};

PyTypeObject KX_BlenderMaterial::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_BlenderMaterial",
	sizeof(PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&CValue::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyObject *KX_BlenderMaterial::pyattr_get_shader(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return self->PygetShader(nullptr, nullptr);
}

static int kx_blender_material_get_textures_size_cb(void *self_v)
{
	return RAS_Texture::MaxUnits;
}

static PyObject *kx_blender_material_get_textures_item_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	PyObject *item = nullptr;
	if (tex) {
		item = tex->GetProxy();
	}
	else {
		item = Py_None;
		Py_INCREF(Py_None);
	}
	return item;
}

static const std::string kx_blender_material_get_textures_item_name_cb(void *self_v, int index)
{
	BL_Texture *tex = (BL_Texture *)((KX_BlenderMaterial *)self_v)->GetTexture(index);
	return (tex ? tex->GetName() : "");
}

PyObject *KX_BlenderMaterial::pyattr_get_textures(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	return (new CListWrapper(self_v,
							 ((KX_BlenderMaterial *)self_v)->GetProxy(),
							 nullptr,
							 kx_blender_material_get_textures_size_cb,
							 kx_blender_material_get_textures_item_cb,
							 kx_blender_material_get_textures_item_name_cb,
							 nullptr))->NewProxy(true);
}

PyObject *KX_BlenderMaterial::pyattr_get_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->alpha);
}

int KX_BlenderMaterial::pyattr_set_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->alpha = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spectra);
}

int KX_BlenderMaterial::pyattr_set_specular_alpha(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spectra = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyLong_FromLong(self->GetBlenderMaterial()->har);
}

int KX_BlenderMaterial::pyattr_set_hardness(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	int val = PyLong_AsLong(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = int: KX_BlenderMaterial, expected a int", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 1, 511);

	self->GetBlenderMaterial()->har = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->spec);
}

int KX_BlenderMaterial::pyattr_set_specular_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->spec = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_specular_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_SPECULAR_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(MT_Vector3(mat->specr, mat->specg, mat->specb));
#endif
}

int KX_BlenderMaterial::pyattr_set_specular_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	Material *mat = self->GetBlenderMaterial();
	mat->specr = color[0];
	mat->specg = color[1];
	mat->specb = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->ref);
}

int KX_BlenderMaterial::pyattr_set_diffuse_intensity(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->ref = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_diffuse_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Color_CreatePyObject_cb(BGE_PROXY_FROM_REF(self_v), mathutils_kxblendermaterial_color_cb_index, MATHUTILS_COL_CB_MATERIAL_DIFFUSE_COLOR);
#else
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	Material *mat = self->GetBlenderMaterial();
	return PyColorFromVector(MT_Vector3(mat->r, mat->g, mat->b));
#endif
}

int KX_BlenderMaterial::pyattr_set_diffuse_color(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	MT_Vector3 color;
	if (!PyVecTo(value, color))
		return PY_SET_ATTR_FAIL;

	Material *mat = self->GetBlenderMaterial();
	mat->r = color[0];
	mat->g = color[1];
	mat->b = color[2];
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->emit);
}

int KX_BlenderMaterial::pyattr_set_emit(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 2.0f);

	self->GetBlenderMaterial()->emit = val;
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_BlenderMaterial::pyattr_get_ambient(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	return PyFloat_FromDouble(self->GetBlenderMaterial()->amb);
}

int KX_BlenderMaterial::pyattr_set_ambient(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_BlenderMaterial *self = static_cast<KX_BlenderMaterial *>(self_v);
	float val = PyFloat_AsDouble(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_AttributeError, "material.%s = float: KX_BlenderMaterial, expected a float", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	CLAMP(val, 0.0f, 1.0f);

	self->GetBlenderMaterial()->amb = val;
	return PY_SET_ATTR_SUCCESS;
}

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, getShader, "getShader()")
{
	if (!m_customShader) {
		m_customShader = new BL_Shader();
		// Set the material to use custom shader.
		m_flag &= ~RAS_BLENDERGLSL;
	}

	return m_customShader->GetProxy();
}

static const unsigned int GL_array[11] = {
	RAS_Rasterizer::RAS_ZERO,
	RAS_Rasterizer::RAS_ONE,
	RAS_Rasterizer::RAS_SRC_COLOR,
	RAS_Rasterizer::RAS_ONE_MINUS_SRC_COLOR,
	RAS_Rasterizer::RAS_DST_COLOR,
	RAS_Rasterizer::RAS_ONE_MINUS_DST_COLOR,
	RAS_Rasterizer::RAS_SRC_ALPHA,
	RAS_Rasterizer::RAS_ONE_MINUS_SRC_ALPHA,
	RAS_Rasterizer::RAS_DST_ALPHA,
	RAS_Rasterizer::RAS_ONE_MINUS_DST_ALPHA,
	RAS_Rasterizer::RAS_SRC_ALPHA_SATURATE
};

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, setBlending, "setBlending(bge.logic.src, bge.logic.dest)")
{
	unsigned int b[2];
	if (PyArg_ParseTuple(args, "ii:setBlending", &b[0], &b[1])) {
		bool value_found[2] = {false, false};
		for (int i = 0; i < 11; i++) {
			if (b[0] == GL_array[i]) {
				value_found[0] = true;
				m_blendFunc[0] = b[0];
			}
			if (b[1] == GL_array[i]) {
				value_found[1] = true;
				m_blendFunc[1] = b[1];
			}
			if (value_found[0] && value_found[1]) {
				break;
			}
		}
		if (!value_found[0] || !value_found[1]) {
			PyErr_SetString(PyExc_ValueError, "material.setBlending(int, int): KX_BlenderMaterial, invalid enum.");
			return nullptr;
		}
		m_userDefBlend = true;
		Py_RETURN_NONE;
	}
	return nullptr;
}

KX_PYMETHODDEF_DOC(KX_BlenderMaterial, getTextureBindcode, "getTextureBindcode(texslot)")
{
	ShowDeprecationWarning("material.getTextureBindcode(texslot)", "material.textures[texslot].bindCode");
	unsigned int texslot;
	if (!PyArg_ParseTuple(args, "i:texslot", &texslot)) {
		PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, expected an int.");
		return nullptr;
	}
	Image *ima = GetTexture(texslot)->GetImage();
	if (ima) {
		unsigned int *bindcode = ima->bindcode;
		return PyLong_FromLong(*bindcode);
	}
	PyErr_SetString(PyExc_ValueError, "material.getTextureBindcode(texslot): KX_BlenderMaterial, invalid texture slot.");
	return nullptr;
}

bool ConvertPythonToMaterial(PyObject *value, KX_BlenderMaterial **material, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*material = nullptr;
		return false;
	}

	if (value == Py_None) {
		*material = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_BlenderMaterial or a KX_BlenderMaterial name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_BlenderMaterial::Type)) {
		KX_BlenderMaterial *mat = static_cast<KX_BlenderMaterial *>BGE_PROXY_REF(value);

		/* sets the error */
		if (mat == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " BGE_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		*material = mat;
		return true;
	}

	*material = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_BlenderMaterial or a string", error_prefix);
	}

	return false;
}

#endif // WITH_PYTHON
