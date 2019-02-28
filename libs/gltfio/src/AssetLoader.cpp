/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gltfio/AssetLoader.h>

#include "FFilamentAsset.h"
#include "GltfEnums.h"
#include "MaterialGenerator.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <math/mat4.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include <utils/EntityManager.h>
#include <utils/Log.h>

#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "upcast.h"

using namespace filament;
using namespace filament::math;
using namespace utils;

namespace gltfio {
namespace details {

// MeshCache
// ---------
// If a given glTF mesh is referenced by multiple glTF nodes, then it generates a separate Filament
// renderable for each of those nodes. All renderables generated by a given mesh share a common set
// of VertexBuffer and IndexBuffer objects. To achieve the sharing behavior, the loader maintains a
// small cache. The cache keys are glTF mesh definitions and the cache entries are lists of
// primitives, where a "primitive" is a reference to a Filament VertexBuffer and IndexBuffer.
struct Primitive {
    VertexBuffer* vertices = nullptr;
    IndexBuffer* indices = nullptr;
    Aabb aabb; // object-space bounding box
};
using Mesh = std::vector<Primitive>;
using MeshCache = tsl::robin_map<const cgltf_mesh*, Mesh>;

// Filament materials are cached by the MaterialGenerator, but material instances are cached here.
using MatInstanceCache = tsl::robin_map<intptr_t, MaterialInstance*>;

// Filament automatically infers the size of driver-level vertex buffers from the attribute data
// (stride, count, offset) and clients are expected to avoid uploading data blobs that exceed this
// size. Since this information doesn't exist in the glTF we need to compute it manually. This is a
// bit of a cheat, cgltf_calc_size is private but its implementation file is available in this cpp
// file.
static uint32_t computeBindingSize(const cgltf_accessor* accessor){
    cgltf_size element_size = cgltf_calc_size(accessor->type, accessor->component_type);
    return uint32_t(accessor->stride * (accessor->count - 1) + element_size);
};

static uint32_t computeBindingOffset(const cgltf_accessor* accessor) {
    return uint32_t(accessor->offset + accessor->buffer_view->offset);
};

struct FAssetLoader : public AssetLoader {
    FAssetLoader(Engine* engine) :
            mEntityManager(EntityManager::get()),
            mRenderableManager(engine->getRenderableManager()),
            mLightManager(engine->getLightManager()),
            mTransformManager(engine->getTransformManager()),
            mMaterials(engine),
            mEngine(engine) {}

    FFilamentAsset* createAssetFromJson(uint8_t const* bytes, uint32_t nbytes);
    FilamentAsset* createAssetFromBinary(uint8_t const* bytes, uint32_t nbytes);

    void destroyAsset(const FFilamentAsset* asset) {
        delete asset;
    }

    void castShadowsByDefault(bool enable) {
        mCastShadows = enable;
    }

    void receiveShadowsByDefault(bool enable) {
        mReceiveShadows = enable;
    }

    size_t getMaterialsCount() const noexcept {
        return mMaterials.getMaterialsCount();
    }

    const Material* const* getMaterials() const noexcept {
        return mMaterials.getMaterials();
    }

    void destroyMaterials() {
        mMaterials.destroyMaterials();
    }

    void createAsset(const cgltf_data* srcAsset);
    void createEntity(const cgltf_node* node, Entity parent);
    void createRenderable(const cgltf_node* node, Entity entity);
    bool createPrimitive(const cgltf_primitive* inPrim, Primitive* outPrim, const UvMap& uvmap);
    MaterialInstance* createMaterialInstance(const cgltf_material* inputMat, UvMap* uvmap,
            bool vertexColor);
    void addTextureBinding(MaterialInstance* materialInstance, const char* parameterName,
            const cgltf_texture* srcTexture, bool srgb);
    void importSkinningData(Skin& dstSkin, const cgltf_skin& srcSkin);
    bool primitiveHasVertexColor(const cgltf_primitive* inPrim) const;

    bool mCastShadows = true;
    bool mReceiveShadows = true;

    EntityManager& mEntityManager;
    RenderableManager& mRenderableManager;
    LightManager& mLightManager;
    TransformManager& mTransformManager;
    MaterialGenerator mMaterials;
    Engine* mEngine;

    // The loader owns a few transient mappings used only for the current asset being loaded.
    FFilamentAsset* mResult;
    MatInstanceCache mMatInstanceCache;
    MeshCache mMeshCache;
    bool mError = false;
};

FILAMENT_UPCAST(AssetLoader)

} // namespace details

using namespace details;

FFilamentAsset* FAssetLoader::createAssetFromJson(uint8_t const* bytes, uint32_t nbytes) {
    cgltf_options options { cgltf_file_type_invalid };
    cgltf_data* sourceAsset;
    cgltf_result result = cgltf_parse(&options, bytes, nbytes, &sourceAsset);
    if (result != cgltf_result_success) {
        return nullptr;
    }
    createAsset(sourceAsset);
    return mResult;
}

FilamentAsset* FAssetLoader::createAssetFromBinary(uint8_t const* bytes, uint32_t nbytes) {
    cgltf_options options { cgltf_file_type_glb };
    cgltf_data* sourceAsset;
    cgltf_result result = cgltf_parse(&options, bytes, nbytes, &sourceAsset);
    if (result != cgltf_result_success) {
        return nullptr;
    }
    createAsset(sourceAsset);
    return mResult;
}

void FAssetLoader::createAsset(const cgltf_data* srcAsset) {
    mResult = new FFilamentAsset(mEngine);
    mResult->mSourceAsset = srcAsset;
    mResult->acquireSourceAsset();

    // If there is no default scene specified, then the default is the first one.
    // It is not an error for a glTF file to have zero scenes.
    const cgltf_scene* scene = srcAsset->scene ? srcAsset->scene : srcAsset->scenes;
    if (!scene) {
        return;
    }

    // Create a single root node with an identity transform as a convenience to the client.
    mResult->mRoot = mEntityManager.create();
    mTransformManager.create(mResult->mRoot);

    // One scene may have multiple root nodes. Recurse down and create an entity for each node.
    cgltf_node** nodes = scene->nodes;
    for (cgltf_size i = 0, len = scene->nodes_count; i < len; ++i) {
        const cgltf_node* root = nodes[i];
        createEntity(root, mResult->mRoot);
    }

    if (mError) {
        delete mResult;
        mResult = nullptr;
    }

    // Copy over joint lists (references to TransformManager components) and create buffer bindings
    // for inverseBindMatrices.
    mResult->mSkins.resize(srcAsset->skins_count);
    for (cgltf_size i = 0, len = srcAsset->skins_count; i < len; ++i) {
        importSkinningData(mResult->mSkins[i], srcAsset->skins[i]);
    }

    // For each skin, build a list of renderables that it affects.
    for (cgltf_size i = 0, len = srcAsset->nodes_count; i < len; ++i) {
        const cgltf_node& node = srcAsset->nodes[i];
        if (node.skin) {
            int skinIndex = node.skin - &srcAsset->skins[0];
            Entity entity = mResult->mNodeMap[&node];
            mResult->mSkins[skinIndex].targets.push_back(entity);
        }
    }

    // We're done with the import, so free up transient bookkeeping resources.
    mMatInstanceCache.clear();
    mMeshCache.clear();
    mError = false;
}

void FAssetLoader::createEntity(const cgltf_node* node, Entity parent) {
    Entity entity = mEntityManager.create();

    // Always create a transform component to reflect the original hierarchy.
    mat4f localTransform;
    if (node->has_matrix) {
        memcpy(&localTransform[0][0], &node->matrix[0], 16 * sizeof(float));
    } else {
        quatf* rotation = (quatf*) &node->rotation[0];
        float3* scale = (float3*) &node->scale[0];
        float3* translation = (float3*) &node->translation[0];
        localTransform = mat4f::compose(*translation, *rotation, *scale);
    }

    auto parentTransform = mTransformManager.getInstance(parent);
    mTransformManager.create(entity, parentTransform, localTransform);

    // Update the asset's entity list and private node mapping.
    mResult->mEntities.push_back(entity);
    mResult->mNodeMap[node] = entity;

    // If the node has a mesh, then create a renderable component.
    if (node->mesh) {
        createRenderable(node, entity);
     }

    for (cgltf_size i = 0, len = node->children_count; i < len; ++i) {
        createEntity(node->children[i], entity);
    }
}

void FAssetLoader::createRenderable(const cgltf_node* node, Entity entity) {
    const cgltf_mesh* mesh = node->mesh;

    // Compute the transform relative to the root.
    auto thisTransform = mTransformManager.getInstance(entity);
    mat4f worldTransform = mTransformManager.getWorldTransform(thisTransform);

    cgltf_size nprims = mesh->primitives_count;
    RenderableManager::Builder builder(nprims);

    // If the mesh is already loaded, obtain the list of Filament VertexBuffer / IndexBuffer
    // objects that were already generated, otherwise allocate a new list.
    auto iter = mMeshCache.find(mesh);
    if (iter == mMeshCache.end()) {
        mMeshCache[mesh].resize(nprims);
    }
    Primitive* outputPrim = mMeshCache[mesh].data();
    const cgltf_primitive* inputPrim = &mesh->primitives[0];

    Aabb aabb;

    // For each prim, create a Filament VertexBuffer, IndexBuffer, and MaterialInstance.
    for (cgltf_size index = 0; index < nprims; ++index, ++outputPrim, ++inputPrim) {
        RenderableManager::PrimitiveType primType;
        if (!getPrimitiveType(inputPrim->type, &primType)) {
            slog.e << "Unsupported primitive type." << io::endl;
        }

        // Create a material instance for this primitive or fetch one from the cache.
        UvMap uvmap;
        bool hasVertexColor = primitiveHasVertexColor(inputPrim);
        MaterialInstance* mi = createMaterialInstance(inputPrim->material, &uvmap, hasVertexColor);
        builder.material(index, mi);

        // Create a Filament VertexBuffer and IndexBuffer for this prim if we haven't already.
        if (!outputPrim->vertices && !createPrimitive(inputPrim, outputPrim, uvmap)) {
            mError = true;
            continue;
        }

        // Expand the object-space bounding box.
        aabb.min = min(outputPrim->aabb.min, aabb.min);
        aabb.max = max(outputPrim->aabb.max, aabb.max);

        // We are not using the optional offset, minIndex, maxIndex, and count arguments when
        // calling geometry() on the builder. It appears that the glTF spec does not have
        // facilities for these parameters, which is not a huge loss since some of the buffer
        // view and accessor features already have this functionality.
        builder.geometry(index, primType, outputPrim->vertices, outputPrim->indices);
    }

    // Expand the world-space bounding box.
    float3 minpt = (worldTransform * float4(aabb.min, 1.0)).xyz;
    float3 maxpt = (worldTransform * float4(aabb.max, 1.0)).xyz;

    mResult->mBoundingBox.min = min(mResult->mBoundingBox.min, minpt);
    mResult->mBoundingBox.max = max(mResult->mBoundingBox.max, maxpt);

    if (node->skin) {
       builder.skinning(node->skin->joints_count);
    }

    builder
        .boundingBox({aabb.min, aabb.max})
        .culling(false) // TODO: enable frustum culling
        .castShadows(mCastShadows)
        .receiveShadows(mReceiveShadows)
        .build(*mEngine, entity);

    // TODO: call builder.blendOrder()
    // TODO: honor mesh->weights and weight_count for morphing
}

bool FAssetLoader::createPrimitive(const cgltf_primitive* inPrim, Primitive* outPrim,
        const UvMap& uvmap) {

    // In glTF, each primitive may or may not have an index buffer. If a primitive does not have an
    // index buffer, we ask the ResourceLoader to generate a trivial index buffer.
    IndexBuffer* indices;
    const cgltf_accessor* indicesAccessor = inPrim->indices;
    if (indicesAccessor) {
        IndexBuffer::Builder ibb;
        ibb.indexCount(indicesAccessor->count);
        IndexBuffer::IndexType indexType;
        if (!getIndexType(indicesAccessor->component_type, &indexType)) {
            utils::slog.e << "Unrecognized index type." << utils::io::endl;
            return false;
        }
        ibb.bufferType(indexType);
        indices = ibb.build(*mEngine);
        const cgltf_buffer_view* bv = indicesAccessor->buffer_view;
        mResult->mBufferBindings.emplace_back(BufferBinding {
            .uri = bv->buffer->uri,
            .totalSize = uint32_t(bv->buffer->size),
            .offset = computeBindingOffset(indicesAccessor),
            .size = computeBindingSize(indicesAccessor),
            .data = &bv->buffer->data,
            .indexBuffer = indices,
            .convertBytesToShorts = indicesAccessor->component_type == cgltf_component_type_r_8u,
            .generateTrivialIndices = false
        });
    } else {
        const cgltf_size vertexCount = inPrim->attributes[0].data->count;
        indices = IndexBuffer::Builder()
            .indexCount(vertexCount)
            .bufferType(IndexBuffer::IndexType::UINT)
            .build(*mEngine);
        mResult->mBufferBindings.emplace_back(BufferBinding {
            .indexBuffer = indices,
            .size = uint32_t(vertexCount * sizeof(uint32_t)),
            .generateTrivialIndices = true
        });
    }

    // We do not necessarily upload all glTF attribute buffers to the GPU. For example, we do not
    // upload tangent vectors in their source format or more than two UV sets. However the buffer
    // count that gets passed to the Builder should be equal to the glTF attribute count because we
    // do not remap the slots.
    VertexBuffer::Builder vbb;
    vbb.bufferCount(inPrim->attributes_count);

    for (int slot = 0; slot < inPrim->attributes_count; slot++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[slot];
        const cgltf_accessor* inputAccessor = inputAttribute.data;

        // At a minimum, surface orientation requires normals to be present in the source data.
        // Here we re-purpose the normals slot to point to the quats that get computed later.
        if (inputAttribute.type == cgltf_attribute_type_normal) {
            vbb.attribute(VertexAttribute::TANGENTS, slot, VertexBuffer::AttributeType::SHORT4);
            vbb.normalized(VertexAttribute::TANGENTS);
            continue;
        }

        // The glTF tangent data is ignored here, but honored in ResourceLoader.
        if (inputAttribute.type == cgltf_attribute_type_tangent) {
            continue;
        }

        // Translate the cgltf attribute enum into a Filament enum and ignore all uv sets
        // that do not have entries in the mapping table.
        VertexAttribute semantic;
        if (!getVertexAttrType(inputAttribute.type, &semantic)) {
            utils::slog.e << "Unrecognized vertex semantic." << utils::io::endl;
            return false;
        }
        UvSet uvset = uvmap[inputAttribute.index];
        if (inputAttribute.type == cgltf_attribute_type_texcoord) {
            switch (uvset) {
                case UV0:
                    semantic = VertexAttribute::UV0;
                    break;
                case UV1:
                    semantic = VertexAttribute::UV1;
                    break;
                case UNUSED:
                    // It is perfectly acceptable to drop unused texture coordinate sets. In fact
                    // this can occur quite frequently, e.g. if the material has attached textures.
                    continue;
            }
        }

        // This will needlessly set the same vertex count multiple times, which should be fine.
        vbb.vertexCount(inputAccessor->count);

        // The positions accessor is required to have min/max properties, use them to expand
        // the bounding box for this primitive.
        if (inputAttribute.type == cgltf_attribute_type_position) {
            const float* minp = &inputAccessor->min[0];
            const float* maxp = &inputAccessor->max[0];
            outPrim->aabb.min = min(outPrim->aabb.min, float3(minp[0], minp[1], minp[2]));
            outPrim->aabb.max = max(outPrim->aabb.max, float3(maxp[0], maxp[1], maxp[2]));
        }

        VertexBuffer::AttributeType atype;
        if (!getElementType(inputAccessor->type, inputAccessor->component_type, &atype)) {
            slog.e << "Unsupported accessor type." << io::endl;
            return false;
        }

        if (inputAccessor->is_sparse) {
            slog.e << "Sparse accessors not yet supported." << io::endl;
            return false;
        }

        // The cgltf library provides a stride value for all accessors, even though they do not
        // exist in the glTF file. It is computed from the type and the stride of the buffer view.
        // As a convenience, cgltf also replaces zero (default) stride with the actual stride.
        vbb.attribute(semantic, slot, atype, 0, inputAccessor->stride);

        if (inputAccessor->normalized) {
            vbb.normalized(semantic);
        }
    }

    VertexBuffer* vertices = mResult->mPrimMap[inPrim] = vbb.build(*mEngine);

    for (int slot = 0; slot < inPrim->attributes_count; slot++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[slot];
        const cgltf_accessor* inputAccessor = inputAttribute.data;
        const cgltf_buffer_view* bv = inputAccessor->buffer_view;
        if (inputAttribute.type == cgltf_attribute_type_normal ||
                inputAttribute.type == cgltf_attribute_type_tangent) {
            continue;
        }
        if (inputAttribute.type == cgltf_attribute_type_texcoord &&
                uvmap[inputAttribute.index] == UNUSED) {
            continue;
        }
        mResult->mBufferBindings.emplace_back(BufferBinding {
            .uri = bv->buffer->uri,
            .totalSize = (uint32_t) bv->buffer->size,
            .bufferIndex = slot,
            .offset = computeBindingOffset(inputAccessor),
            .size = computeBindingSize(inputAccessor),
            .data = &bv->buffer->data,
            .vertexBuffer = vertices,
            .indexBuffer = nullptr,
            .convertBytesToShorts = false,
            .generateTrivialIndices = false
        });
    }

    outPrim->indices = indices;
    outPrim->vertices = vertices;
    return true;
}

MaterialInstance* FAssetLoader::createMaterialInstance(const cgltf_material* inputMat,
        UvMap* uvmap, bool vertexColor) {
    intptr_t key = ((intptr_t) inputMat) ^ (vertexColor ? 1 : 0);
    auto iter = mMatInstanceCache.find(key);
    if (iter != mMatInstanceCache.end()) {
        return iter->second;
    }

    // The default glTF material is non-lit black.
    if (inputMat == nullptr) {
        MaterialKey matkey {
            .unlit = true
        };
        Material* mat = mMaterials.getOrCreateMaterial(&matkey, uvmap);
        MaterialInstance* mi = mat->createInstance();
        mResult->mMaterialInstances.push_back(mi);
        return mMatInstanceCache[0] = mi;
    }

    if (inputMat->has_pbr_specular_glossiness) {
        slog.e << "KHR_materials_pbrSpecularGlossiness is not supported." << io::endl;
    }

    auto pbrConfig = inputMat->pbr_metallic_roughness;
    bool hasTextureTransforms = pbrConfig.base_color_texture.has_transform ||
        pbrConfig.metallic_roughness_texture.has_transform ||
        inputMat->normal_texture.has_transform ||
        inputMat->occlusion_texture.has_transform ||
        inputMat->emissive_texture.has_transform;

    MaterialKey matkey {
        .doubleSided = (bool) inputMat->double_sided,
        .unlit = (bool) inputMat->unlit,
        .hasVertexColors = vertexColor,
        .hasBaseColorTexture = pbrConfig.base_color_texture.texture,
        .hasMetallicRoughnessTexture = pbrConfig.metallic_roughness_texture.texture,
        .hasNormalTexture = inputMat->normal_texture.texture,
        .hasOcclusionTexture = inputMat->occlusion_texture.texture,
        .hasEmissiveTexture = inputMat->emissive_texture.texture,
        .alphaMode = AlphaMode::OPAQUE,
        .alphaMaskThreshold = 0.5f,
        .baseColorUV = (uint8_t) pbrConfig.base_color_texture.texcoord,
        .metallicRoughnessUV = (uint8_t) pbrConfig.metallic_roughness_texture.texcoord,
        .emissiveUV = (uint8_t) inputMat->emissive_texture.texcoord,
        .aoUV = (uint8_t) inputMat->occlusion_texture.texcoord,
        .normalUV = (uint8_t) inputMat->normal_texture.texcoord,
        .hasTextureTransforms = hasTextureTransforms
    };

    switch (inputMat->alpha_mode) {
        case cgltf_alpha_mode_opaque:
            matkey.alphaMode = AlphaMode::OPAQUE;
            break;
        case cgltf_alpha_mode_mask:
            matkey.alphaMode = AlphaMode::MASKED;
            matkey.alphaMaskThreshold = inputMat->alpha_cutoff;
            break;
        case cgltf_alpha_mode_blend:
            matkey.alphaMode = AlphaMode::TRANSPARENT;
            break;
    }

    // This not only creates (or fetches) a material, it modifies the material key according to
    // our rendering constraints. For example, Filament only supports 2 sets of texture coordinates.
    Material* mat = mMaterials.getOrCreateMaterial(&matkey, uvmap);

    // Create an instance of the material that has a unique set of texture bindings etc.
    MaterialInstance* mi = mat->createInstance();
    mResult->mMaterialInstances.push_back(mi);

    const float* e = &inputMat->emissive_factor[0];
    mi->setParameter("emissiveFactor", float3(e[0], e[1], e[2]));
    mi->setParameter("normalScale", inputMat->normal_texture.scale);
    mi->setParameter("aoStrength", inputMat->occlusion_texture.scale);

    const float* c = &pbrConfig.base_color_factor[0];
    mi->setParameter("baseColorFactor", float4(c[0], c[1], c[2], c[3]));
    mi->setParameter("metallicFactor", pbrConfig.metallic_factor);
    mi->setParameter("roughnessFactor", pbrConfig.roughness_factor);

    if (matkey.hasBaseColorTexture) {
        addTextureBinding(mi, "baseColorMap", pbrConfig.base_color_texture.texture, true);
        if (matkey.hasTextureTransforms) {
            const cgltf_texture_transform& uvt = pbrConfig.base_color_texture.transform;
            // float offset[2], cgltf_float rotation, float scale[2];
            // auto uvmat = matrixFromUvTransform(uvt.offset, uvt.rotation, uvt.scale);
            mi->setParameter("baseColorUvMatrix", mat3f());
        }
    }

    if (matkey.hasMetallicRoughnessTexture) {
        addTextureBinding(mi, "metallicRoughnessMap",
                pbrConfig.metallic_roughness_texture.texture, false);
        if (matkey.hasTextureTransforms) {
            mi->setParameter("metallicRoughnessUvMatrix", mat3f());
        }
    }

    if (matkey.hasNormalTexture) {
        addTextureBinding(mi, "normalMap", inputMat->normal_texture.texture, false);
        if (matkey.hasTextureTransforms) {
            mi->setParameter("normalUvMatrix", mat3f());
        }
    }

    if (matkey.hasOcclusionTexture) {
        addTextureBinding(mi, "occlusionMap", inputMat->occlusion_texture.texture, false);
        if (matkey.hasTextureTransforms) {
            mi->setParameter("occlusionUvMatrix", mat3f());
        }
    }

    if (matkey.hasEmissiveTexture) {
        addTextureBinding(mi, "emissiveMap", inputMat->emissive_texture.texture, true);
        if (matkey.hasTextureTransforms) {
            mi->setParameter("emissiveUvMatrix", mat3f());
        }
    }

    return mMatInstanceCache[key] = mi;
}

void FAssetLoader::addTextureBinding(MaterialInstance* materialInstance, const char* parameterName,
        const cgltf_texture* srcTexture, bool srgb) {
    if (!srcTexture->image) {
        slog.w << "Texture is missing image (" << srcTexture->name << ")." << io::endl;
        return;
    }
    TextureSampler dstSampler;
    auto srcSampler = srcTexture->sampler;
    if (srcSampler) {
        dstSampler.setWrapModeS(getWrapMode(srcSampler->wrap_s));
        dstSampler.setWrapModeT(getWrapMode(srcSampler->wrap_t));
        dstSampler.setMagFilter(getMagFilter(srcSampler->mag_filter));
        dstSampler.setMinFilter(getMinFilter(srcSampler->min_filter));
    } else {
        // These defaults are stipulated by the spec:
        dstSampler.setWrapModeS(TextureSampler::WrapMode::REPEAT);
        dstSampler.setWrapModeT(TextureSampler::WrapMode::REPEAT);

        // These defaults are up the implementation but since we generate mipmaps unconditionally,
        // we might as well use them. In practice the conformance models look awful without
        // using mipmapping by default.
        dstSampler.setMagFilter(TextureSampler::MagFilter::LINEAR);
        dstSampler.setMinFilter(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR);
    }
    auto bv = srcTexture->image->buffer_view;
    mResult->mTextureBindings.push_back(TextureBinding {
        .uri = srcTexture->image->uri,
        .totalSize = uint32_t(bv ? bv->buffer->size : 0),
        .mimeType = srcTexture->image->mime_type,
        .data = bv ? &bv->buffer->data : nullptr,
        .materialInstance = materialInstance,
        .materialParameter = parameterName,
        .sampler = dstSampler,
        .srgb = srgb
    });
}

void FAssetLoader::importSkinningData(Skin& dstSkin, const cgltf_skin& srcSkin) {
    if (srcSkin.name) {
        dstSkin.name = srcSkin.name;
    }
    dstSkin.joints.resize(srcSkin.joints_count);
    const auto& nodeMap = mResult->mNodeMap;
    for (cgltf_size i = 0, len = srcSkin.joints_count; i < len; ++i) {
        dstSkin.joints[i] = nodeMap.at(srcSkin.joints[i]);
    }
}

bool FAssetLoader::primitiveHasVertexColor(const cgltf_primitive* inPrim) const {
    for (int slot = 0; slot < inPrim->attributes_count; slot++) {
        const cgltf_attribute& inputAttribute = inPrim->attributes[slot];
        if (inputAttribute.type == cgltf_attribute_type_color) {
            return true;
        }
    }
    return false;
}

AssetLoader* AssetLoader::create(Engine* engine) {
    return new FAssetLoader(engine);
}

void AssetLoader::destroy(AssetLoader** loader) {
    delete *loader;
    *loader = nullptr;
}

FilamentAsset* AssetLoader::createAssetFromJson(uint8_t const* bytes, uint32_t nbytes) {
    return upcast(this)->createAssetFromJson(bytes, nbytes);
}

FilamentAsset* AssetLoader::createAssetFromBinary(uint8_t const* bytes, uint32_t nbytes) {
    return upcast(this)->createAssetFromBinary(bytes, nbytes);
}

void AssetLoader::destroyAsset(const FilamentAsset* asset) {
    upcast(this)->destroyAsset(upcast(asset));
}

void AssetLoader::castShadowsByDefault(bool enable) {
    upcast(this)->castShadowsByDefault(enable);
}

void AssetLoader::receiveShadowsByDefault(bool enable) {
    upcast(this)->receiveShadowsByDefault(enable);
}

size_t AssetLoader::getMaterialsCount() const noexcept {
    return upcast(this)->getMaterialsCount();
}

const Material* const* AssetLoader::getMaterials() const noexcept {
    return upcast(this)->getMaterials();
}

void AssetLoader::destroyMaterials() {
    upcast(this)->destroyMaterials();
}

} // namespace gltfio
