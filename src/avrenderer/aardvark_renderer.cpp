#include "aardvark_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <chrono>
#include <map>
#include <set>
#include "algorithm"
#include <filesystem>

#if defined(__ANDROID__)
#define TINYGLTF_ANDROID_LOAD_FROM_ASSETS
#endif

#include <vulkan/vulkan.h>
#include "VulkanExampleBase.h"
#include "VulkanTexture.hpp"
#include "VulkanglTFModel.hpp"
#include "VulkanUtils.hpp"
#include "ui.hpp"

#include "include/cef_sandbox_win.h"
#include "av_cef_app.h"
#include "av_cef_handler.h"

// When generating projects with CMake the CEF_USE_SANDBOX value will be defined
// automatically if using the required compiler version. Pass -DUSE_SANDBOX=OFF
// to the CMake command-line to disable use of the sandbox.
// Uncomment this line to manually enable sandbox support.
// #define CEF_USE_SANDBOX 1

#if defined(CEF_USE_SANDBOX)
// The cef_sandbox.lib static library may not link successfully with all VS
// versions.
#pragma comment(lib, "cef_sandbox.lib")
#endif



#include <tools/pathtools.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>


using namespace aardvark;

void UpdateTransformable( std::shared_ptr<vkglTF::Transformable> pTransformable, AvTransform::Reader & transform )
{
	if ( transform.hasPosition() )
	{
		pTransformable->translation.x = transform.getPosition().getX();
		pTransformable->translation.y = transform.getPosition().getY();
		pTransformable->translation.z = transform.getPosition().getZ();
	}
	else
	{
		pTransformable->translation = glm::vec3( 0.f );
	}

	if ( transform.hasScale() )
	{
		pTransformable->scale.x = transform.getScale().getX();
		pTransformable->scale.y = transform.getScale().getY();
		pTransformable->scale.z = transform.getScale().getZ();
	}
	else
	{
		pTransformable->scale = glm::vec3( 1.f );
	}

	if ( transform.hasRotation() )
	{
		pTransformable->rotation.x = transform.getRotation().getX();
		pTransformable->rotation.y = transform.getRotation().getY();
		pTransformable->rotation.z = transform.getRotation().getZ();
		pTransformable->rotation.w = transform.getRotation().getW();
	}
	else
	{
		pTransformable->rotation = glm::quat();
	}
}


VulkanExample::VulkanExample()
	: VulkanExampleBase()
{
	title = "Aardvark Renderer";
#if defined(TINYGLTF_ENABLE_DRACO)
	std::cout << "Draco mesh compression is enabled" << std::endl;
#endif
}

VulkanExample::~VulkanExample() noexcept
{
	m_pClient->Stop();
	m_pClient = nullptr;

	vkDestroyPipeline( device, pipelines.skybox, nullptr );
	vkDestroyPipeline( device, pipelines.pbr, nullptr );
	vkDestroyPipeline( device, pipelines.pbrAlphaBlend, nullptr );

	vkDestroyPipelineLayout( device, pipelineLayout, nullptr );

	m_mapModels.clear();

	for ( auto buffer : uniformBuffers )
	{
		buffer.params.destroy();
		buffer.scene.destroy();
		buffer.skybox.destroy();
		buffer.leftEye.destroy();
		buffer.rightEye.destroy();
	}
	for ( auto fence : waitFences ) {
		vkDestroyFence( device, fence, nullptr );
	}
	for ( auto semaphore : renderCompleteSemaphores ) {
		vkDestroySemaphore( device, semaphore, nullptr );
	}
	for ( auto semaphore : presentCompleteSemaphores ) {
		vkDestroySemaphore( device, semaphore, nullptr );
	}

	textures.environmentCube.destroy();
	textures.irradianceCube.destroy();
	textures.prefilteredCube.destroy();
	textures.lutBrdf.destroy();
	textures.empty.destroy();

	delete ui;
}

void VulkanExample::renderNode( std::shared_ptr<vkglTF::Model> pModel, std::shared_ptr<vkglTF::Node> node, uint32_t cbIndex, vkglTF::Material::AlphaMode alphaMode, EEye eEye )
{
	if ( node->mesh ) {
		// Render mesh primitives
		for ( auto primitive : node->mesh->primitives ) {
			vkglTF::Material & primitiveMaterial = primitive->materialIndex >= pModel->materials.size()
				? pModel->materials.back() : pModel->materials[primitive->materialIndex];

			if ( primitiveMaterial.alphaMode == alphaMode ) {

				VkDescriptorSet descriptorSet;
				switch ( eEye )
				{
				case EEye::Left:
					descriptorSet = descriptorSets[cbIndex].eye[vr::Eye_Left]->set();
					break;
				case EEye::Right:
					descriptorSet = descriptorSets[cbIndex].eye[vr::Eye_Right]->set();
					break;
				default:
				case EEye::Mirror:
					descriptorSet = descriptorSets[cbIndex].scene->set();
					break;
				}

				const std::vector<VkDescriptorSet> descriptorsets =
				{
					descriptorSet,
					primitiveMaterial.descriptorSet->set(),
					node->mesh->uniformBuffer.descriptor->set(),
				};
				vkCmdBindDescriptorSets( commandBuffers[cbIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, static_cast<uint32_t>( descriptorsets.size() ), descriptorsets.data(), 0, NULL );


				// Pass material parameters as push constants
				PushConstBlockMaterial pushConstBlockMaterial{};
				pushConstBlockMaterial.emissiveFactor = primitiveMaterial.emissiveFactor;
				// To save push constant space, availability and texture coordinate set are combined
				// -1 = texture not used for this material, >= 0 texture used and index of texture coordinate set
				pushConstBlockMaterial.colorTextureSet = primitiveMaterial.baseColorTexture != nullptr ? primitiveMaterial.texCoordSets.baseColor : -1;
				pushConstBlockMaterial.normalTextureSet = primitiveMaterial.normalTexture != nullptr ? primitiveMaterial.texCoordSets.normal : -1;
				pushConstBlockMaterial.occlusionTextureSet = primitiveMaterial.occlusionTexture != nullptr ? primitiveMaterial.texCoordSets.occlusion : -1;
				pushConstBlockMaterial.emissiveTextureSet = primitiveMaterial.emissiveTexture != nullptr ? primitiveMaterial.texCoordSets.emissive : -1;
				pushConstBlockMaterial.alphaMask = static_cast<float>( primitiveMaterial.alphaMode == vkglTF::Material::ALPHAMODE_MASK );
				pushConstBlockMaterial.alphaMaskCutoff = primitiveMaterial.alphaCutoff;

				// TODO: glTF specs states that metallic roughness should be preferred, even if specular glossiness is present

				switch ( primitiveMaterial.workflow )
				{
				case vkglTF::Material::Workflow::MetallicRoughness:
					pushConstBlockMaterial.workflow = static_cast<float>( PBR_WORKFLOW_METALLIC_ROUGHNESS );
					pushConstBlockMaterial.baseColorFactor = primitiveMaterial.baseColorFactor;
					pushConstBlockMaterial.metallicFactor = primitiveMaterial.metallicFactor;
					pushConstBlockMaterial.roughnessFactor = primitiveMaterial.roughnessFactor;
					pushConstBlockMaterial.PhysicalDescriptorTextureSet = primitiveMaterial.metallicRoughnessTexture != nullptr ? primitiveMaterial.texCoordSets.metallicRoughness : -1;
					pushConstBlockMaterial.colorTextureSet = primitiveMaterial.baseColorTexture != nullptr ? primitiveMaterial.texCoordSets.baseColor : -1;
					break;

				case vkglTF::Material::Workflow::SpecularGlossiness:
					pushConstBlockMaterial.workflow = static_cast<float>( PBR_WORKFLOW_SPECULAR_GLOSINESS );
					pushConstBlockMaterial.PhysicalDescriptorTextureSet = primitiveMaterial.extension.specularGlossinessTexture != nullptr ? primitiveMaterial.texCoordSets.specularGlossiness : -1;
					pushConstBlockMaterial.colorTextureSet = primitiveMaterial.extension.diffuseTexture != nullptr ? primitiveMaterial.texCoordSets.baseColor : -1;
					pushConstBlockMaterial.diffuseFactor = primitiveMaterial.extension.diffuseFactor;
					pushConstBlockMaterial.specularFactor = glm::vec4( primitiveMaterial.extension.specularFactor, 1.0f );
					break;

				case vkglTF::Material::Workflow::Unlit:
					pushConstBlockMaterial.workflow = static_cast<float>( PBR_WORKFLOW_UNLIT );
					pushConstBlockMaterial.baseColorFactor = primitiveMaterial.baseColorFactor;
					break;
				}

				vkCmdPushConstants( commandBuffers[cbIndex], pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( PushConstBlockMaterial ), &pushConstBlockMaterial );

				PushConstBlockVertex pushConstVertex{};
				pushConstVertex.uvScaleAndOffset =
				{
					primitiveMaterial.baseColorScale[0], primitiveMaterial.baseColorScale[1],
					primitiveMaterial.baseColorOffset[0], primitiveMaterial.baseColorOffset[1]
				};

				vkCmdPushConstants( commandBuffers[cbIndex], pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof( PushConstBlockMaterial ), sizeof( PushConstBlockVertex ), &pushConstVertex );

				if ( primitive->hasIndices ) {
					vkCmdDrawIndexed( commandBuffers[cbIndex], primitive->indexCount, 1, primitive->firstIndex, 0, 0 );
				}
				else {
					vkCmdDraw( commandBuffers[cbIndex], primitive->vertexCount, 1, 0, 0 );
				}
			}
		}

	};
	for ( auto child : node->children ) {
		renderNode( pModel, child, cbIndex, alphaMode, eEye );
	}
}

void VulkanExample::recordCommandBuffers( uint32_t cbIndex )
{
	VkCommandBufferBeginInfo cmdBufferBeginInfo{};
	cmdBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VkCommandBuffer currentCB = commandBuffers[cbIndex];

	VK_CHECK_RESULT( vkBeginCommandBuffer( currentCB, &cmdBufferBeginInfo ) );

	renderScene( cbIndex, renderPass, frameBuffers[cbIndex], width, height, EEye::Mirror );
	renderSceneToTarget( cbIndex, leftEyeRT, eyeWidth, eyeHeight, EEye::Left );
	renderSceneToTarget( cbIndex, rightEyeRT, eyeWidth, eyeHeight, EEye::Right );

	VK_CHECK_RESULT( vkEndCommandBuffer( currentCB ) );
}

void VulkanExample::renderSceneToTarget( uint32_t cbIndex, vks::RenderTarget target, uint32_t targetWidth, uint32_t targetHeight, EEye eEye )
{
	VkCommandBuffer currentCB = commandBuffers[cbIndex];

	target.transitionColorLayout( currentCB, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );

	renderScene( cbIndex, target.renderPass, target.frameBuffer, targetWidth, targetHeight, eEye );

	target.transitionColorLayout( currentCB, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
}

void VulkanExample::renderScene( uint32_t cbIndex, VkRenderPass targetRenderPass, VkFramebuffer targetFrameBuffer, uint32_t targetWidth, uint32_t targetHeight, EEye eEye )
{

	VkClearValue clearValues[3];
	if ( settings.multiSampling ) {
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[2].depthStencil = { 1.0f, 0 };
	}
	else {
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };
	}

	VkRenderPassBeginInfo renderPassBeginInfo{};
	VkCommandBuffer currentCB = commandBuffers[cbIndex];

	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.renderPass = targetRenderPass;
	renderPassBeginInfo.renderArea.offset.x = 0;
	renderPassBeginInfo.renderArea.offset.y = 0;
	renderPassBeginInfo.renderArea.extent.width = targetWidth;
	renderPassBeginInfo.renderArea.extent.height = targetHeight;
	renderPassBeginInfo.clearValueCount = settings.multiSampling ? 3 : 2;
	renderPassBeginInfo.pClearValues = clearValues;


	renderPassBeginInfo.framebuffer = targetFrameBuffer;


	vkCmdBeginRenderPass( currentCB, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE );

	VkViewport viewport{};
	viewport.width = (float)targetWidth;
	viewport.height = (float)targetHeight;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport( currentCB, 0, 1, &viewport );

	VkRect2D scissor{};
	scissor.extent = { targetWidth, targetHeight };
	vkCmdSetScissor( currentCB, 0, 1, &scissor );

	//if (displayBackground) {
	//	vkCmdBindDescriptorSets(currentCB, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[i].skybox, 0, nullptr);
	//	vkCmdBindPipeline(currentCB, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.skybox);
	//	models.skybox.draw(currentCB);
	//}

//		if( !bDebug )
	vkCmdBindPipeline( currentCB, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.pbr );


	recordCommandsForModels( currentCB, cbIndex, vkglTF::Material::ALPHAMODE_OPAQUE, eEye );
	recordCommandsForModels( currentCB, cbIndex, vkglTF::Material::ALPHAMODE_MASK, eEye );

	// Transparent primitives
	// TODO: Correct depth sorting
	vkCmdBindPipeline( currentCB, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.pbrAlphaBlend );
	recordCommandsForModels( currentCB, cbIndex, vkglTF::Material::ALPHAMODE_BLEND, eEye );

	if ( eEye == EEye::Mirror )
	{
		// User interface
		ui->draw( currentCB );
	}

	vkCmdEndRenderPass( currentCB );
}

void VulkanExample::recordCommandsForModels( VkCommandBuffer currentCB, uint32_t i, vkglTF::Material::AlphaMode eAlphaMode, EEye eEye )
{
	for ( auto pModel : m_vecModelsToRender )
	{
		VkDeviceSize offsets[1] = { 0 };
		vkCmdBindVertexBuffers( currentCB, 0, 1, &pModel->buffers->vertices.buffer, offsets );
		if ( pModel->buffers->indices.buffer != VK_NULL_HANDLE )
		{
			vkCmdBindIndexBuffer( currentCB, pModel->buffers->indices.buffer, 0, VK_INDEX_TYPE_UINT32 );
		}

		for ( auto node : pModel->nodes ) {
			renderNode( pModel, node, i, eAlphaMode, eEye );
		}
	}
}


void VulkanExample::loadEnvironment( std::string filename )
{
	std::cout << "Loading environment from " << filename << std::endl;
	if ( textures.environmentCube.image ) {
		textures.environmentCube.destroy();
		textures.irradianceCube.destroy();
		textures.prefilteredCube.destroy();
	}
	textures.environmentCube.loadFromFile( filename, VK_FORMAT_R16G16B16A16_SFLOAT, vulkanDevice, queue );
	generateCubemaps();
}

void VulkanExample::loadAssets()
{
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	tinygltf::asset_manager = androidApp->activity->assetManager;
	readDirectory( assetpath + "models", "*.gltf", scenes, true );
#else
	const std::string assetpath = std::string( VK_EXAMPLE_DATA_DIR ) + "/";
	struct stat info;
	if ( stat( assetpath.c_str(), &info ) != 0 ) {
		std::string msg = "Could not locate asset path in \"" + assetpath + "\".\nMake sure binary is run from correct relative directory!";
		std::cerr << msg << std::endl;
		exit( -1 );
	}
#endif
	readDirectory( assetpath + "environments", "*.ktx", environments, false );

	textures.empty.loadFromFile( assetpath + "textures/empty.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue );

	std::string sceneFile = assetpath + "models/DamagedHelmet/glTF-Embedded/DamagedHelmet.gltf";
	std::string envMapFile = assetpath + "environments/papermill.ktx";
	for ( size_t i = 0; i < args.size(); i++ ) {
		if ( std::string( args[i] ).find( ".gltf" ) != std::string::npos ) {
			std::ifstream file( args[i] );
			if ( file.good() ) {
				sceneFile = args[i];
			}
			else {
				std::cout << "could not load \"" << args[i] << "\"" << std::endl;
			}
		}
		if ( std::string( args[i] ).find( ".ktx" ) != std::string::npos ) {
			std::ifstream file( args[i] );
			if ( file.good() ) {
				envMapFile = args[i];
			}
			else {
				std::cout << "could not load \"" << args[i] << "\"" << std::endl;
			}
		}
	}

	//loadScene(sceneFile.c_str());
	//models.skybox.loadFromFile(assetpath + "models/Box/glTF-Embedded/Box.gltf", vulkanDevice, queue);

	m_skybox.loadFromFile( assetpath + "models/Box/glTF-Embedded/Box.gltf", vulkanDevice, m_descriptorManager, queue );
	loadEnvironment( envMapFile.c_str() );

}

void VulkanExample::UpdateDescriptorForScene( VkDescriptorSet descriptorSet, VkBuffer buffer, uint32_t bufferSize )
{
	VkDescriptorBufferInfo bufferInfo = { buffer, 0, bufferSize };

	std::array<VkWriteDescriptorSet, 5> writeDescriptorSets{};

	writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writeDescriptorSets[0].descriptorCount = 1;
	writeDescriptorSets[0].dstSet = descriptorSet;
	writeDescriptorSets[0].dstBinding = 0;
	writeDescriptorSets[0].pBufferInfo = &bufferInfo;

	writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writeDescriptorSets[1].descriptorCount = 1;
	writeDescriptorSets[1].dstSet = descriptorSet;
	writeDescriptorSets[1].dstBinding = 1;
	writeDescriptorSets[1].pBufferInfo = &bufferInfo;

	writeDescriptorSets[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSets[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writeDescriptorSets[2].descriptorCount = 1;
	writeDescriptorSets[2].dstSet = descriptorSet;
	writeDescriptorSets[2].dstBinding = 2;
	writeDescriptorSets[2].pImageInfo = &textures.irradianceCube.descriptor;

	writeDescriptorSets[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSets[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writeDescriptorSets[3].descriptorCount = 1;
	writeDescriptorSets[3].dstSet = descriptorSet;
	writeDescriptorSets[3].dstBinding = 3;
	writeDescriptorSets[3].pImageInfo = &textures.prefilteredCube.descriptor;

	writeDescriptorSets[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSets[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writeDescriptorSets[4].descriptorCount = 1;
	writeDescriptorSets[4].dstSet = descriptorSet;
	writeDescriptorSets[4].dstBinding = 4;
	writeDescriptorSets[4].pImageInfo = &textures.lutBrdf.descriptor;

	vkUpdateDescriptorSets( device, static_cast<uint32_t>( writeDescriptorSets.size() ), writeDescriptorSets.data(), 0, NULL );
}

void VulkanExample::setupDescriptors()
{

	/*
		Descriptor sets
	*/

	// Scene (matrices and environment maps)
	{

		for ( auto i = 0; i < descriptorSets.size(); i++ )
		{
			auto fnUpdateDescriptor =
				[this, i]( vks::VulkanDevice *vulkanDevice, vks::CDescriptorSet *descriptor )
			{
				UpdateDescriptorForScene( descriptor->set(),
					uniformBuffers[i].scene.buffer,
					uniformBuffers[i].scene.size );
			};

			descriptorSets[i].scene = m_descriptorManager->createDescriptorSet( fnUpdateDescriptor, vks::EDescriptorLayout::Scene );

			auto fnUpdateDescriptorLeftEye =
				[this, i]( vks::VulkanDevice *vulkanDevice, vks::CDescriptorSet *descriptor )
			{
				UpdateDescriptorForScene( descriptor->set(),
					uniformBuffers[i].leftEye.buffer,
					uniformBuffers[i].leftEye.size );
			};
			descriptorSets[i].eye[vr::Eye_Left] = m_descriptorManager->createDescriptorSet( fnUpdateDescriptorLeftEye, vks::EDescriptorLayout::Scene );

			auto fnUpdateDescriptorRightEye =
				[this, i]( vks::VulkanDevice *vulkanDevice, vks::CDescriptorSet *descriptor )
			{
				UpdateDescriptorForScene( descriptor->set(),
					uniformBuffers[i].rightEye.buffer,
					uniformBuffers[i].rightEye.size );
			};
			descriptorSets[i].eye[vr::Eye_Right] = m_descriptorManager->createDescriptorSet( fnUpdateDescriptorRightEye, vks::EDescriptorLayout::Scene );
		}
	}

	// Material (samplers)
	// Per-Material descriptor sets
	for ( auto iModel : m_mapModels )
	{
		setupDescriptorSetsForModel( iModel.second );
	}

	// Skybox (fixed set)
	for ( auto i = 0; i < uniformBuffers.size(); i++ )
	{
		descriptorSets[i].skybox = m_descriptorManager->createDescriptorSet(
			[this, i]( vks::VulkanDevice *vulkanDevice, vks::CDescriptorSet *desc )
		{
			std::array<VkWriteDescriptorSet, 3> writeDescriptorSets{};

			writeDescriptorSets[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writeDescriptorSets[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writeDescriptorSets[0].descriptorCount = 1;
			writeDescriptorSets[0].dstSet = desc->set();
			writeDescriptorSets[0].dstBinding = 0;
			writeDescriptorSets[0].pBufferInfo = &uniformBuffers[i].skybox.descriptor;

			writeDescriptorSets[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writeDescriptorSets[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writeDescriptorSets[1].descriptorCount = 1;
			writeDescriptorSets[1].dstSet = desc->set();
			writeDescriptorSets[1].dstBinding = 1;
			writeDescriptorSets[1].pBufferInfo = &uniformBuffers[i].params.descriptor;

			writeDescriptorSets[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writeDescriptorSets[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writeDescriptorSets[2].descriptorCount = 1;
			writeDescriptorSets[2].dstSet = desc->set();
			writeDescriptorSets[2].dstBinding = 2;
			writeDescriptorSets[2].pImageInfo = &textures.prefilteredCube.descriptor;

			vkUpdateDescriptorSets( device, static_cast<uint32_t>( writeDescriptorSets.size() ), writeDescriptorSets.data(), 0, nullptr );
		}, vks::EDescriptorLayout::Scene );
	}
}

void VulkanExample::setupDescriptorSetsForModel( std::shared_ptr<vkglTF::Model> pModel )
{
	for ( auto &material : pModel->materials )
	{
		material.descriptorSet = m_descriptorManager->createDescriptorSet( [this, material]( vks::VulkanDevice *vulkanDevice, vks::CDescriptorSet *desc )
		{
			std::vector<VkDescriptorImageInfo> imageDescriptors =
			{
				textures.empty.descriptor,
				textures.empty.descriptor,
				material.normalTexture ? material.normalTexture->descriptor : textures.empty.descriptor,
				material.occlusionTexture ? material.occlusionTexture->descriptor : textures.empty.descriptor,
				material.emissiveTexture ? material.emissiveTexture->descriptor : textures.empty.descriptor
			};

			switch ( material.workflow )
			{
			case vkglTF::Material::Workflow::MetallicRoughness:
				// TODO: glTF specs states that metallic roughness should be preferred, even if specular glossiness is present
				if ( material.baseColorTexture )
				{
					imageDescriptors[0] = material.baseColorTexture->descriptor;
				}
				if ( material.metallicRoughnessTexture )
				{
					imageDescriptors[1] = material.metallicRoughnessTexture->descriptor;
				}
				break;

			case vkglTF::Material::Workflow::SpecularGlossiness:
				if ( material.extension.diffuseTexture )
				{
					imageDescriptors[0] = material.extension.diffuseTexture->descriptor;
				}
				if ( material.extension.specularGlossinessTexture )
				{
					imageDescriptors[1] = material.extension.specularGlossinessTexture->descriptor;
				}
				break;

			case vkglTF::Material::Workflow::Unlit:
				if ( material.baseColorTexture )
				{
					imageDescriptors[0] = material.baseColorTexture->descriptor;
				}
				break;
			}

			std::array<VkWriteDescriptorSet, 5> writeDescriptorSets{};
			for ( size_t i = 0; i < imageDescriptors.size(); i++ )
			{
				writeDescriptorSets[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writeDescriptorSets[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writeDescriptorSets[i].descriptorCount = 1;
				writeDescriptorSets[i].dstSet = desc->set();
				writeDescriptorSets[i].dstBinding = static_cast<uint32_t>( i );
				writeDescriptorSets[i].pImageInfo = &imageDescriptors[i];
			}

			vkUpdateDescriptorSets( device, static_cast<uint32_t>( writeDescriptorSets.size() ), writeDescriptorSets.data(), 0, NULL );
		}, vks::EDescriptorLayout::Material );
	}
}

void VulkanExample::preparePipelines()
{
	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI{};
	inputAssemblyStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssemblyStateCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineRasterizationStateCreateInfo rasterizationStateCI{};
	rasterizationStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCI.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationStateCI.cullMode = VK_CULL_MODE_BACK_BIT;
	rasterizationStateCI.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizationStateCI.lineWidth = 1.0f;

	VkPipelineColorBlendAttachmentState blendAttachmentState{};
	blendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachmentState.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCI{};
	colorBlendStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCI.attachmentCount = 1;
	colorBlendStateCI.pAttachments = &blendAttachmentState;

	VkPipelineDepthStencilStateCreateInfo depthStencilStateCI{};
	depthStencilStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilStateCI.depthTestEnable = VK_FALSE;
	depthStencilStateCI.depthWriteEnable = VK_FALSE;
	depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencilStateCI.front = depthStencilStateCI.back;
	depthStencilStateCI.back.compareOp = VK_COMPARE_OP_ALWAYS;

	VkPipelineViewportStateCreateInfo viewportStateCI{};
	viewportStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCI.viewportCount = 1;
	viewportStateCI.scissorCount = 1;

	VkPipelineMultisampleStateCreateInfo multisampleStateCI{};
	multisampleStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;

	if ( settings.multiSampling ) {
		multisampleStateCI.rasterizationSamples = settings.sampleCount;
	}

	std::vector<VkDynamicState> dynamicStateEnables = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicStateCI{};
	dynamicStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicStateCI.pDynamicStates = dynamicStateEnables.data();
	dynamicStateCI.dynamicStateCount = static_cast<uint32_t>( dynamicStateEnables.size() );

	// Pipeline layout
	const std::vector<VkDescriptorSetLayout> setLayouts =
	{
		m_descriptorManager->getLayout( vks::EDescriptorLayout::Scene ),
		m_descriptorManager->getLayout( vks::EDescriptorLayout::Material ),
		m_descriptorManager->getLayout( vks::EDescriptorLayout::Node ),
	};

	std::array<VkPushConstantRange, 2> arrayConstantRanges{};
	arrayConstantRanges[0].size = sizeof( PushConstBlockMaterial );
	arrayConstantRanges[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	arrayConstantRanges[1].size = sizeof( PushConstBlockVertex );
	arrayConstantRanges[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	arrayConstantRanges[1].offset = sizeof( PushConstBlockMaterial );

	VkPipelineLayoutCreateInfo pipelineLayoutCI{};
	pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCI.setLayoutCount = static_cast<uint32_t>( setLayouts.size() );
	pipelineLayoutCI.pSetLayouts = setLayouts.data();
	pipelineLayoutCI.pushConstantRangeCount = (uint32_t)arrayConstantRanges.size();
	pipelineLayoutCI.pPushConstantRanges = arrayConstantRanges.data();
	VK_CHECK_RESULT( vkCreatePipelineLayout( device, &pipelineLayoutCI, nullptr, &pipelineLayout ) );

	// Vertex bindings an attributes
	VkVertexInputBindingDescription vertexInputBinding = { 0, sizeof( vkglTF::Model::Vertex ), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof( float ) * 3 },
		{ 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof( float ) * 6 },
		{ 3, 0, VK_FORMAT_R32G32_SFLOAT, sizeof( float ) * 8 },
		{ 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof( float ) * 10 },
		{ 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof( float ) * 14 }
	};
	VkPipelineVertexInputStateCreateInfo vertexInputStateCI{};
	vertexInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputStateCI.vertexBindingDescriptionCount = 1;
	vertexInputStateCI.pVertexBindingDescriptions = &vertexInputBinding;
	vertexInputStateCI.vertexAttributeDescriptionCount = static_cast<uint32_t>( vertexInputAttributes.size() );
	vertexInputStateCI.pVertexAttributeDescriptions = vertexInputAttributes.data();

	// Pipelines
	std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.layout = pipelineLayout;
	pipelineCI.renderPass = renderPass;
	pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
	pipelineCI.pVertexInputState = &vertexInputStateCI;
	pipelineCI.pRasterizationState = &rasterizationStateCI;
	pipelineCI.pColorBlendState = &colorBlendStateCI;
	pipelineCI.pMultisampleState = &multisampleStateCI;
	pipelineCI.pViewportState = &viewportStateCI;
	pipelineCI.pDepthStencilState = &depthStencilStateCI;
	pipelineCI.pDynamicState = &dynamicStateCI;
	pipelineCI.stageCount = static_cast<uint32_t>( shaderStages.size() );
	pipelineCI.pStages = shaderStages.data();

	if ( settings.multiSampling ) {
		multisampleStateCI.rasterizationSamples = settings.sampleCount;
	}

	// Skybox pipeline (background cube)
	shaderStages = {
		loadShader( device, "skybox.vert.spv", VK_SHADER_STAGE_VERTEX_BIT ),
		loadShader( device, "skybox.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT )
	};
	VK_CHECK_RESULT( vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.skybox ) );
	for ( auto shaderStage : shaderStages ) {
		vkDestroyShaderModule( device, shaderStage.module, nullptr );
	}

	// PBR pipeline
	shaderStages = {
		loadShader( device, "pbr.vert.spv", VK_SHADER_STAGE_VERTEX_BIT ),
		loadShader( device, "pbr_khr.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT )
	};
	depthStencilStateCI.depthWriteEnable = VK_TRUE;
	depthStencilStateCI.depthTestEnable = VK_TRUE;
	VK_CHECK_RESULT( vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.pbr ) );

	rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
	blendAttachmentState.blendEnable = VK_TRUE;
	blendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachmentState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendAttachmentState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
	blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
	VK_CHECK_RESULT( vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.pbrAlphaBlend ) );

	for ( auto shaderStage : shaderStages ) {
		vkDestroyShaderModule( device, shaderStage.module, nullptr );
	}
}

/*
	Generate a BRDF integration map storing roughness/NdotV as a look-up-table
*/
void VulkanExample::generateBRDFLUT()
{
	auto tStart = std::chrono::high_resolution_clock::now();

	const VkFormat format = VK_FORMAT_R16G16_SFLOAT;
	const int32_t dim = 512;

	// Image
	VkImageCreateInfo imageCI{};
	imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCI.imageType = VK_IMAGE_TYPE_2D;
	imageCI.format = format;
	imageCI.extent.width = dim;
	imageCI.extent.height = dim;
	imageCI.extent.depth = 1;
	imageCI.mipLevels = 1;
	imageCI.arrayLayers = 1;
	imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
	imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VK_CHECK_RESULT( vkCreateImage( device, &imageCI, nullptr, &textures.lutBrdf.image ) );
	printf( "Image 0x%llX function %s\n", (size_t)textures.lutBrdf.image, __FUNCTION__ );

	VkMemoryRequirements memReqs;
	vkGetImageMemoryRequirements( device, textures.lutBrdf.image, &memReqs );
	VkMemoryAllocateInfo memAllocInfo{};
	memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memAllocInfo.allocationSize = memReqs.size;
	memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK_RESULT( vkAllocateMemory( device, &memAllocInfo, nullptr, &textures.lutBrdf.deviceMemory ) );
	VK_CHECK_RESULT( vkBindImageMemory( device, textures.lutBrdf.image, textures.lutBrdf.deviceMemory, 0 ) );

	// View
	VkImageViewCreateInfo viewCI{};
	viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewCI.format = format;
	viewCI.subresourceRange = {};
	viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewCI.subresourceRange.levelCount = 1;
	viewCI.subresourceRange.layerCount = 1;
	viewCI.image = textures.lutBrdf.image;
	VK_CHECK_RESULT( vkCreateImageView( device, &viewCI, nullptr, &textures.lutBrdf.view ) );

	// Sampler
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.minLod = 0.0f;
	samplerCI.maxLod = 1.0f;
	samplerCI.maxAnisotropy = 1.0f;
	samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	VK_CHECK_RESULT( vkCreateSampler( device, &samplerCI, nullptr, &textures.lutBrdf.sampler ) );

	// FB, Att, RP, Pipe, etc.
	VkAttachmentDescription attDesc{};
	// Color attachment
	attDesc.format = format;
	attDesc.samples = VK_SAMPLE_COUNT_1_BIT;
	attDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attDesc.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

	VkSubpassDescription subpassDescription{};
	subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.colorAttachmentCount = 1;
	subpassDescription.pColorAttachments = &colorReference;

	// Use subpass dependencies for layout transitions
	std::array<VkSubpassDependency, 2> dependencies;
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	// Create the actual renderpass
	VkRenderPassCreateInfo renderPassCI{};
	renderPassCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassCI.attachmentCount = 1;
	renderPassCI.pAttachments = &attDesc;
	renderPassCI.subpassCount = 1;
	renderPassCI.pSubpasses = &subpassDescription;
	renderPassCI.dependencyCount = 2;
	renderPassCI.pDependencies = dependencies.data();

	VkRenderPass renderpass;
	VK_CHECK_RESULT( vkCreateRenderPass( device, &renderPassCI, nullptr, &renderpass ) );

	VkFramebufferCreateInfo framebufferCI{};
	framebufferCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferCI.renderPass = renderpass;
	framebufferCI.attachmentCount = 1;
	framebufferCI.pAttachments = &textures.lutBrdf.view;
	framebufferCI.width = dim;
	framebufferCI.height = dim;
	framebufferCI.layers = 1;

	VkFramebuffer framebuffer;
	VK_CHECK_RESULT( vkCreateFramebuffer( device, &framebufferCI, nullptr, &framebuffer ) );

	// Desriptors
	VkDescriptorSetLayout descriptorsetlayout;
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI{};
	descriptorSetLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	VK_CHECK_RESULT( vkCreateDescriptorSetLayout( device, &descriptorSetLayoutCI, nullptr, &descriptorsetlayout ) );

	// Pipeline layout
	VkPipelineLayout pipelinelayout;
	VkPipelineLayoutCreateInfo pipelineLayoutCI{};
	pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutCI.setLayoutCount = 1;
	pipelineLayoutCI.pSetLayouts = &descriptorsetlayout;
	VK_CHECK_RESULT( vkCreatePipelineLayout( device, &pipelineLayoutCI, nullptr, &pipelinelayout ) );

	// Pipeline
	VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI{};
	inputAssemblyStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssemblyStateCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineRasterizationStateCreateInfo rasterizationStateCI{};
	rasterizationStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizationStateCI.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
	rasterizationStateCI.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizationStateCI.lineWidth = 1.0f;

	VkPipelineColorBlendAttachmentState blendAttachmentState{};
	blendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachmentState.blendEnable = VK_FALSE;

	VkPipelineColorBlendStateCreateInfo colorBlendStateCI{};
	colorBlendStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlendStateCI.attachmentCount = 1;
	colorBlendStateCI.pAttachments = &blendAttachmentState;

	VkPipelineDepthStencilStateCreateInfo depthStencilStateCI{};
	depthStencilStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilStateCI.depthTestEnable = VK_FALSE;
	depthStencilStateCI.depthWriteEnable = VK_FALSE;
	depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencilStateCI.front = depthStencilStateCI.back;
	depthStencilStateCI.back.compareOp = VK_COMPARE_OP_ALWAYS;

	VkPipelineViewportStateCreateInfo viewportStateCI{};
	viewportStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportStateCI.viewportCount = 1;
	viewportStateCI.scissorCount = 1;

	VkPipelineMultisampleStateCreateInfo multisampleStateCI{};
	multisampleStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampleStateCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicStateCI{};
	dynamicStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicStateCI.pDynamicStates = dynamicStateEnables.data();
	dynamicStateCI.dynamicStateCount = static_cast<uint32_t>( dynamicStateEnables.size() );

	VkPipelineVertexInputStateCreateInfo emptyInputStateCI{};
	emptyInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.layout = pipelinelayout;
	pipelineCI.renderPass = renderpass;
	pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
	pipelineCI.pVertexInputState = &emptyInputStateCI;
	pipelineCI.pRasterizationState = &rasterizationStateCI;
	pipelineCI.pColorBlendState = &colorBlendStateCI;
	pipelineCI.pMultisampleState = &multisampleStateCI;
	pipelineCI.pViewportState = &viewportStateCI;
	pipelineCI.pDepthStencilState = &depthStencilStateCI;
	pipelineCI.pDynamicState = &dynamicStateCI;
	pipelineCI.stageCount = 2;
	pipelineCI.pStages = shaderStages.data();

	// Look-up-table (from BRDF) pipeline		
	shaderStages = {
		loadShader( device, "genbrdflut.vert.spv", VK_SHADER_STAGE_VERTEX_BIT ),
		loadShader( device, "genbrdflut.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT )
	};
	VkPipeline pipeline;
	VK_CHECK_RESULT( vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline ) );
	for ( auto shaderStage : shaderStages ) {
		vkDestroyShaderModule( device, shaderStage.module, nullptr );
	}

	// Render
	VkClearValue clearValues[1];
	clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	VkRenderPassBeginInfo renderPassBeginInfo{};
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.renderPass = renderpass;
	renderPassBeginInfo.renderArea.extent.width = dim;
	renderPassBeginInfo.renderArea.extent.height = dim;
	renderPassBeginInfo.clearValueCount = 1;
	renderPassBeginInfo.pClearValues = clearValues;
	renderPassBeginInfo.framebuffer = framebuffer;

	VkCommandBuffer cmdBuf = vulkanDevice->createCommandBuffer( VK_COMMAND_BUFFER_LEVEL_PRIMARY, true );
	vkCmdBeginRenderPass( cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE );

	VkViewport viewport{};
	viewport.width = (float)dim;
	viewport.height = (float)dim;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;

	VkRect2D scissor{};
	scissor.extent.width = width;
	scissor.extent.height = height;

	vkCmdSetViewport( cmdBuf, 0, 1, &viewport );
	vkCmdSetScissor( cmdBuf, 0, 1, &scissor );
	vkCmdBindPipeline( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
	vkCmdDraw( cmdBuf, 3, 1, 0, 0 );
	vkCmdEndRenderPass( cmdBuf );
	vulkanDevice->flushCommandBuffer( cmdBuf, queue );

	vkQueueWaitIdle( queue );

	vkDestroyPipeline( device, pipeline, nullptr );
	vkDestroyPipelineLayout( device, pipelinelayout, nullptr );
	vkDestroyRenderPass( device, renderpass, nullptr );
	vkDestroyFramebuffer( device, framebuffer, nullptr );
	vkDestroyDescriptorSetLayout( device, descriptorsetlayout, nullptr );

	textures.lutBrdf.descriptor.imageView = textures.lutBrdf.view;
	textures.lutBrdf.descriptor.sampler = textures.lutBrdf.sampler;
	textures.lutBrdf.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	textures.lutBrdf.device = vulkanDevice;

	auto tEnd = std::chrono::high_resolution_clock::now();
	auto tDiff = std::chrono::duration<double, std::milli>( tEnd - tStart ).count();
	std::cout << "Generating BRDF LUT took " << tDiff << " ms" << std::endl;
}

/*
	Offline generation for the cube maps used for PBR lighting
	- Irradiance cube map
	- Pre-filterd environment cubemap
*/
void VulkanExample::generateCubemaps()
{
	enum Target { IRRADIANCE = 0, PREFILTEREDENV = 1 };

	for ( uint32_t target = 0; target < PREFILTEREDENV + 1; target++ ) {

		vks::TextureCubeMap cubemap;

		auto tStart = std::chrono::high_resolution_clock::now();

		VkFormat format;
		int32_t dim;

		switch ( target ) {
		case IRRADIANCE:
			format = VK_FORMAT_R32G32B32A32_SFLOAT;
			dim = 64;
			break;
		case PREFILTEREDENV:
			format = VK_FORMAT_R16G16B16A16_SFLOAT;
			dim = 512;
			break;
		};

		const uint32_t numMips = static_cast<uint32_t>( floor( log2( dim ) ) ) + 1;

		// Create target cubemap
		{
			// Image
			VkImageCreateInfo imageCI{};
			imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageCI.imageType = VK_IMAGE_TYPE_2D;
			imageCI.format = format;
			imageCI.extent.width = dim;
			imageCI.extent.height = dim;
			imageCI.extent.depth = 1;
			imageCI.mipLevels = numMips;
			imageCI.arrayLayers = 6;
			imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
			imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageCI.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			imageCI.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			VK_CHECK_RESULT( vkCreateImage( device, &imageCI, nullptr, &cubemap.image ) );
			printf( "Image 0x%llX function %s\n", (size_t)cubemap.image, __FUNCTION__ );

			VkMemoryRequirements memReqs;
			vkGetImageMemoryRequirements( device, cubemap.image, &memReqs );
			VkMemoryAllocateInfo memAllocInfo{};
			memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			memAllocInfo.allocationSize = memReqs.size;
			memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
			VK_CHECK_RESULT( vkAllocateMemory( device, &memAllocInfo, nullptr, &cubemap.deviceMemory ) );
			VK_CHECK_RESULT( vkBindImageMemory( device, cubemap.image, cubemap.deviceMemory, 0 ) );

			// View
			VkImageViewCreateInfo viewCI{};
			viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewCI.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
			viewCI.format = format;
			viewCI.subresourceRange = {};
			viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewCI.subresourceRange.levelCount = numMips;
			viewCI.subresourceRange.layerCount = 6;
			viewCI.image = cubemap.image;
			VK_CHECK_RESULT( vkCreateImageView( device, &viewCI, nullptr, &cubemap.view ) );

			// Sampler
			VkSamplerCreateInfo samplerCI{};
			samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerCI.magFilter = VK_FILTER_LINEAR;
			samplerCI.minFilter = VK_FILTER_LINEAR;
			samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerCI.minLod = 0.0f;
			samplerCI.maxLod = static_cast<float>( numMips );
			samplerCI.maxAnisotropy = 1.0f;
			samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			VK_CHECK_RESULT( vkCreateSampler( device, &samplerCI, nullptr, &cubemap.sampler ) );
		}

		// FB, Att, RP, Pipe, etc.
		VkAttachmentDescription attDesc{};
		// Color attachment
		attDesc.format = format;
		attDesc.samples = VK_SAMPLE_COUNT_1_BIT;
		attDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attDesc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpassDescription{};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pColorAttachments = &colorReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		// Renderpass
		VkRenderPassCreateInfo renderPassCI{};
		renderPassCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassCI.attachmentCount = 1;
		renderPassCI.pAttachments = &attDesc;
		renderPassCI.subpassCount = 1;
		renderPassCI.pSubpasses = &subpassDescription;
		renderPassCI.dependencyCount = 2;
		renderPassCI.pDependencies = dependencies.data();
		VkRenderPass renderpass;
		VK_CHECK_RESULT( vkCreateRenderPass( device, &renderPassCI, nullptr, &renderpass ) );

		struct Offscreen {
			VkImage image;
			VkImageView view;
			VkDeviceMemory memory;
			VkFramebuffer framebuffer;
		} offscreen;

		// Create offscreen framebuffer
		{
			// Image
			VkImageCreateInfo imageCI{};
			imageCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageCI.imageType = VK_IMAGE_TYPE_2D;
			imageCI.format = format;
			imageCI.extent.width = dim;
			imageCI.extent.height = dim;
			imageCI.extent.depth = 1;
			imageCI.mipLevels = 1;
			imageCI.arrayLayers = 1;
			imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
			imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageCI.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VK_CHECK_RESULT( vkCreateImage( device, &imageCI, nullptr, &offscreen.image ) );
			printf( "Image 0x%llX function %s\n", (size_t)offscreen.image, __FUNCTION__ );

			VkMemoryRequirements memReqs;
			vkGetImageMemoryRequirements( device, offscreen.image, &memReqs );
			VkMemoryAllocateInfo memAllocInfo{};
			memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			memAllocInfo.allocationSize = memReqs.size;
			memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
			VK_CHECK_RESULT( vkAllocateMemory( device, &memAllocInfo, nullptr, &offscreen.memory ) );
			VK_CHECK_RESULT( vkBindImageMemory( device, offscreen.image, offscreen.memory, 0 ) );

			// View
			VkImageViewCreateInfo viewCI{};
			viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewCI.format = format;
			viewCI.flags = 0;
			viewCI.subresourceRange = {};
			viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewCI.subresourceRange.baseMipLevel = 0;
			viewCI.subresourceRange.levelCount = 1;
			viewCI.subresourceRange.baseArrayLayer = 0;
			viewCI.subresourceRange.layerCount = 1;
			viewCI.image = offscreen.image;
			VK_CHECK_RESULT( vkCreateImageView( device, &viewCI, nullptr, &offscreen.view ) );

			// Framebuffer
			VkFramebufferCreateInfo framebufferCI{};
			framebufferCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferCI.renderPass = renderpass;
			framebufferCI.attachmentCount = 1;
			framebufferCI.pAttachments = &offscreen.view;
			framebufferCI.width = dim;
			framebufferCI.height = dim;
			framebufferCI.layers = 1;
			VK_CHECK_RESULT( vkCreateFramebuffer( device, &framebufferCI, nullptr, &offscreen.framebuffer ) );

			VkCommandBuffer layoutCmd = vulkanDevice->createCommandBuffer( VK_COMMAND_BUFFER_LEVEL_PRIMARY, true );
			VkImageMemoryBarrier imageMemoryBarrier{};
			imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imageMemoryBarrier.image = offscreen.image;
			imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			imageMemoryBarrier.srcAccessMask = 0;
			imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			imageMemoryBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier( layoutCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier );
			vulkanDevice->flushCommandBuffer( layoutCmd, queue, true );
		}

		// Descriptors
		VkDescriptorSetLayout descriptorsetlayout;
		VkDescriptorSetLayoutBinding setLayoutBinding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI{};
		descriptorSetLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptorSetLayoutCI.pBindings = &setLayoutBinding;
		descriptorSetLayoutCI.bindingCount = 1;
		VK_CHECK_RESULT( vkCreateDescriptorSetLayout( device, &descriptorSetLayoutCI, nullptr, &descriptorsetlayout ) );

		// Descriptor Pool
		VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
		VkDescriptorPoolCreateInfo descriptorPoolCI{};
		descriptorPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolCI.poolSizeCount = 1;
		descriptorPoolCI.pPoolSizes = &poolSize;
		descriptorPoolCI.maxSets = 2;
		VkDescriptorPool descriptorpool;
		VK_CHECK_RESULT( vkCreateDescriptorPool( device, &descriptorPoolCI, nullptr, &descriptorpool ) );

		// Descriptor sets
		VkDescriptorSet descriptorset;
		VkDescriptorSetAllocateInfo descriptorSetAllocInfo{};
		descriptorSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAllocInfo.descriptorPool = descriptorpool;
		descriptorSetAllocInfo.pSetLayouts = &descriptorsetlayout;
		descriptorSetAllocInfo.descriptorSetCount = 1;
		VK_CHECK_RESULT( vkAllocateDescriptorSets( device, &descriptorSetAllocInfo, &descriptorset ) );
		VkWriteDescriptorSet writeDescriptorSet{};
		writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writeDescriptorSet.descriptorCount = 1;
		writeDescriptorSet.dstSet = descriptorset;
		writeDescriptorSet.dstBinding = 0;
		writeDescriptorSet.pImageInfo = &textures.environmentCube.descriptor;
		vkUpdateDescriptorSets( device, 1, &writeDescriptorSet, 0, nullptr );

		struct PushBlockIrradiance {
			glm::mat4 mvp;
			float deltaPhi = ( 2.0f * float( M_PI ) ) / 180.0f;
			float deltaTheta = ( 0.5f * float( M_PI ) ) / 64.0f;
		} pushBlockIrradiance;

		struct PushBlockPrefilterEnv {
			glm::mat4 mvp;
			float roughness;
			uint32_t numSamples = 32u;
		} pushBlockPrefilterEnv;

		// Pipeline layout
		VkPipelineLayout pipelinelayout;
		VkPushConstantRange pushConstantRange{};
		pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

		switch ( target ) {
		case IRRADIANCE:
			pushConstantRange.size = sizeof( PushBlockIrradiance );
			break;
		case PREFILTEREDENV:
			pushConstantRange.size = sizeof( PushBlockPrefilterEnv );
			break;
		};

		VkPipelineLayoutCreateInfo pipelineLayoutCI{};
		pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutCI.setLayoutCount = 1;
		pipelineLayoutCI.pSetLayouts = &descriptorsetlayout;
		pipelineLayoutCI.pushConstantRangeCount = 1;
		pipelineLayoutCI.pPushConstantRanges = &pushConstantRange;
		VK_CHECK_RESULT( vkCreatePipelineLayout( device, &pipelineLayoutCI, nullptr, &pipelinelayout ) );

		// Pipeline
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI{};
		inputAssemblyStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssemblyStateCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineRasterizationStateCreateInfo rasterizationStateCI{};
		rasterizationStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizationStateCI.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
		rasterizationStateCI.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizationStateCI.lineWidth = 1.0f;

		VkPipelineColorBlendAttachmentState blendAttachmentState{};
		blendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		blendAttachmentState.blendEnable = VK_FALSE;

		VkPipelineColorBlendStateCreateInfo colorBlendStateCI{};
		colorBlendStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlendStateCI.attachmentCount = 1;
		colorBlendStateCI.pAttachments = &blendAttachmentState;

		VkPipelineDepthStencilStateCreateInfo depthStencilStateCI{};
		depthStencilStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencilStateCI.depthTestEnable = VK_FALSE;
		depthStencilStateCI.depthWriteEnable = VK_FALSE;
		depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		depthStencilStateCI.front = depthStencilStateCI.back;
		depthStencilStateCI.back.compareOp = VK_COMPARE_OP_ALWAYS;

		VkPipelineViewportStateCreateInfo viewportStateCI{};
		viewportStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportStateCI.viewportCount = 1;
		viewportStateCI.scissorCount = 1;

		VkPipelineMultisampleStateCreateInfo multisampleStateCI{};
		multisampleStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampleStateCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicStateCI{};
		dynamicStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicStateCI.pDynamicStates = dynamicStateEnables.data();
		dynamicStateCI.dynamicStateCount = static_cast<uint32_t>( dynamicStateEnables.size() );

		// Vertex input state
		VkVertexInputBindingDescription vertexInputBinding = { 0, sizeof( vkglTF::Model::Vertex ), VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription vertexInputAttribute = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };

		VkPipelineVertexInputStateCreateInfo vertexInputStateCI{};
		vertexInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputStateCI.vertexBindingDescriptionCount = 1;
		vertexInputStateCI.pVertexBindingDescriptions = &vertexInputBinding;
		vertexInputStateCI.vertexAttributeDescriptionCount = 1;
		vertexInputStateCI.pVertexAttributeDescriptions = &vertexInputAttribute;

		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI{};
		pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCI.layout = pipelinelayout;
		pipelineCI.renderPass = renderpass;
		pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
		pipelineCI.pVertexInputState = &vertexInputStateCI;
		pipelineCI.pRasterizationState = &rasterizationStateCI;
		pipelineCI.pColorBlendState = &colorBlendStateCI;
		pipelineCI.pMultisampleState = &multisampleStateCI;
		pipelineCI.pViewportState = &viewportStateCI;
		pipelineCI.pDepthStencilState = &depthStencilStateCI;
		pipelineCI.pDynamicState = &dynamicStateCI;
		pipelineCI.stageCount = 2;
		pipelineCI.pStages = shaderStages.data();
		pipelineCI.renderPass = renderpass;

		shaderStages[0] = loadShader( device, "filtercube.vert.spv", VK_SHADER_STAGE_VERTEX_BIT );
		switch ( target ) {
		case IRRADIANCE:
			shaderStages[1] = loadShader( device, "irradiancecube.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT );
			break;
		case PREFILTEREDENV:
			shaderStages[1] = loadShader( device, "prefilterenvmap.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT );
			break;
		};
		VkPipeline pipeline;
		VK_CHECK_RESULT( vkCreateGraphicsPipelines( device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline ) );
		for ( auto shaderStage : shaderStages ) {
			vkDestroyShaderModule( device, shaderStage.module, nullptr );
		}

		// Render cubemap
		VkClearValue clearValues[1];
		clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 0.0f } };

		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = renderpass;
		renderPassBeginInfo.framebuffer = offscreen.framebuffer;
		renderPassBeginInfo.renderArea.extent.width = dim;
		renderPassBeginInfo.renderArea.extent.height = dim;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = clearValues;

		std::vector<glm::mat4> matrices = {
			glm::rotate( glm::rotate( glm::mat4( 1.0f ), glm::radians( 90.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) ), glm::radians( 180.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ),
			glm::rotate( glm::rotate( glm::mat4( 1.0f ), glm::radians( -90.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) ), glm::radians( 180.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ),
			glm::rotate( glm::mat4( 1.0f ), glm::radians( -90.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ),
			glm::rotate( glm::mat4( 1.0f ), glm::radians( 90.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ),
			glm::rotate( glm::mat4( 1.0f ), glm::radians( 180.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ),
			glm::rotate( glm::mat4( 1.0f ), glm::radians( 180.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) ),
		};

		VkCommandBuffer cmdBuf = vulkanDevice->createCommandBuffer( VK_COMMAND_BUFFER_LEVEL_PRIMARY, false );

		VkViewport viewport{};
		viewport.width = (float)dim;
		viewport.height = (float)dim;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor{};
		scissor.extent.width = width;
		scissor.extent.height = height;

		VkImageSubresourceRange subresourceRange{};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = numMips;
		subresourceRange.layerCount = 6;

		// Change image layout for all cubemap faces to transfer destination
		{
			vulkanDevice->beginCommandBuffer( cmdBuf );
			VkImageMemoryBarrier imageMemoryBarrier{};
			imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imageMemoryBarrier.image = cubemap.image;
			imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarrier.srcAccessMask = 0;
			imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.subresourceRange = subresourceRange;
			vkCmdPipelineBarrier( cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier );
			vulkanDevice->flushCommandBuffer( cmdBuf, queue, false );
		}

		for ( uint32_t m = 0; m < numMips; m++ ) {
			for ( uint32_t f = 0; f < 6; f++ ) {

				vulkanDevice->beginCommandBuffer( cmdBuf );

				viewport.width = static_cast<float>( dim * std::pow( 0.5f, m ) );
				viewport.height = static_cast<float>( dim * std::pow( 0.5f, m ) );
				vkCmdSetViewport( cmdBuf, 0, 1, &viewport );
				vkCmdSetScissor( cmdBuf, 0, 1, &scissor );

				// Render scene from cube face's point of view
				vkCmdBeginRenderPass( cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE );

				// Pass parameters for current pass using a push constant block
				switch ( target ) {
				case IRRADIANCE:
					pushBlockIrradiance.mvp = glm::perspective( (float)( M_PI / 2.0 ), 1.0f, 0.1f, 512.0f ) * matrices[f];
					vkCmdPushConstants( cmdBuf, pipelinelayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( PushBlockIrradiance ), &pushBlockIrradiance );
					break;
				case PREFILTEREDENV:
					pushBlockPrefilterEnv.mvp = glm::perspective( (float)( M_PI / 2.0 ), 1.0f, 0.1f, 512.0f ) * matrices[f];
					pushBlockPrefilterEnv.roughness = (float)m / (float)( numMips - 1 );
					vkCmdPushConstants( cmdBuf, pipelinelayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( PushBlockPrefilterEnv ), &pushBlockPrefilterEnv );
					break;
				};

				vkCmdBindPipeline( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
				vkCmdBindDescriptorSets( cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelinelayout, 0, 1, &descriptorset, 0, NULL );

				VkDeviceSize offsets[1] = { 0 };

				m_skybox.draw( cmdBuf );

				vkCmdEndRenderPass( cmdBuf );

				VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				subresourceRange.baseMipLevel = 0;
				subresourceRange.levelCount = numMips;
				subresourceRange.layerCount = 6;

				{
					VkImageMemoryBarrier imageMemoryBarrier{};
					imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					imageMemoryBarrier.image = offscreen.image;
					imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					imageMemoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
					imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
					imageMemoryBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
					vkCmdPipelineBarrier( cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier );
				}

				// Copy region for transfer from framebuffer to cube face
				VkImageCopy copyRegion{};

				copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.srcSubresource.baseArrayLayer = 0;
				copyRegion.srcSubresource.mipLevel = 0;
				copyRegion.srcSubresource.layerCount = 1;
				copyRegion.srcOffset = { 0, 0, 0 };

				copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				copyRegion.dstSubresource.baseArrayLayer = f;
				copyRegion.dstSubresource.mipLevel = m;
				copyRegion.dstSubresource.layerCount = 1;
				copyRegion.dstOffset = { 0, 0, 0 };

				copyRegion.extent.width = static_cast<uint32_t>( viewport.width );
				copyRegion.extent.height = static_cast<uint32_t>( viewport.height );
				copyRegion.extent.depth = 1;

				vkCmdCopyImage(
					cmdBuf,
					offscreen.image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					cubemap.image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1,
					&copyRegion );

				{
					VkImageMemoryBarrier imageMemoryBarrier{};
					imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					imageMemoryBarrier.image = offscreen.image;
					imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
					imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
					imageMemoryBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
					imageMemoryBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
					vkCmdPipelineBarrier( cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier );
				}

				vulkanDevice->flushCommandBuffer( cmdBuf, queue, false );
			}
		}

		{
			vulkanDevice->beginCommandBuffer( cmdBuf );
			VkImageMemoryBarrier imageMemoryBarrier{};
			imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imageMemoryBarrier.image = cubemap.image;
			imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.dstAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.subresourceRange = subresourceRange;
			vkCmdPipelineBarrier( cmdBuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier );
			vulkanDevice->flushCommandBuffer( cmdBuf, queue, false );
		}


		vkDestroyRenderPass( device, renderpass, nullptr );
		vkDestroyFramebuffer( device, offscreen.framebuffer, nullptr );
		vkFreeMemory( device, offscreen.memory, nullptr );
		vkDestroyImageView( device, offscreen.view, nullptr );
		vkDestroyImage( device, offscreen.image, nullptr );
		vkDestroyDescriptorPool( device, descriptorpool, nullptr );
		vkDestroyDescriptorSetLayout( device, descriptorsetlayout, nullptr );
		vkDestroyPipeline( device, pipeline, nullptr );
		vkDestroyPipelineLayout( device, pipelinelayout, nullptr );

		cubemap.descriptor.imageView = cubemap.view;
		cubemap.descriptor.sampler = cubemap.sampler;
		cubemap.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		cubemap.device = vulkanDevice;

		switch ( target ) {
		case IRRADIANCE:
			textures.irradianceCube = cubemap;
			break;
		case PREFILTEREDENV:
			textures.prefilteredCube = cubemap;
			shaderValuesParams.prefilteredCubeMipLevels = static_cast<float>( numMips );
			break;
		};

		auto tEnd = std::chrono::high_resolution_clock::now();
		auto tDiff = std::chrono::duration<double, std::milli>( tEnd - tStart ).count();
		std::cout << "Generating cube map with " << numMips << " mip levels took " << tDiff << " ms" << std::endl;
	}
}

/*
	Prepare and initialize uniform buffers containing shader parameters
*/
void VulkanExample::prepareUniformBuffers()
{
	for ( auto &uniformBuffer : uniformBuffers ) {
		uniformBuffer.scene.create( vulkanDevice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sizeof( shaderValuesScene ) );
		uniformBuffer.skybox.create( vulkanDevice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sizeof( shaderValuesSkybox ) );
		uniformBuffer.params.create( vulkanDevice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sizeof( shaderValuesParams ) );
		uniformBuffer.leftEye.create( vulkanDevice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sizeof( shaderValuesLeftEye ) );
		uniformBuffer.rightEye.create( vulkanDevice, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, sizeof( shaderValuesRightEye ) );
	}
	updateUniformBuffers();
}

void VulkanExample::updateUniformBuffers()
{
	// Scene
	shaderValuesScene.matProjectionFromView = camera.matrices.perspective;
	shaderValuesScene.matViewFromHmd = camera.matrices.view;

	// Center and scale model
	glm::mat4 aabb( 1.f );
	float scale = ( 1.0f / std::max( aabb[0][0], std::max( aabb[1][1], aabb[2][2] ) ) ) * 0.5f;
	glm::vec3 translate = -glm::vec3( aabb[3][0], aabb[3][1], aabb[3][2] );
	translate += -0.5f * glm::vec3( aabb[0][0], aabb[1][1], aabb[2][2] );

	shaderValuesScene.matHmdFromStage = glm::mat4( 1.0f );
	shaderValuesScene.matHmdFromStage[0][0] = scale;
	shaderValuesScene.matHmdFromStage[1][1] = scale;
	shaderValuesScene.matHmdFromStage[2][2] = scale;
	shaderValuesScene.matHmdFromStage = glm::translate( shaderValuesScene.matHmdFromStage, translate );

	shaderValuesScene.camPos = glm::vec3(
		-camera.position.z * sin( glm::radians( camera.rotation.y ) ) * cos( glm::radians( camera.rotation.x ) ),
		-camera.position.z * sin( glm::radians( camera.rotation.x ) ),
		camera.position.z * cos( glm::radians( camera.rotation.y ) ) * cos( glm::radians( camera.rotation.x ) )
	);

	// Skybox
	shaderValuesSkybox.matProjectionFromView = camera.matrices.perspective;
	shaderValuesSkybox.matViewFromHmd = shaderValuesScene.matProjectionFromView;
	shaderValuesSkybox.matHmdFromStage = glm::mat4( glm::mat3( camera.matrices.view ) );

	// left eye
	shaderValuesLeftEye.matProjectionFromView = GetHMDMatrixProjectionEye( vr::Eye_Left );
	shaderValuesLeftEye.matViewFromHmd = GetHMDMatrixPoseEye( vr::Eye_Left );
	shaderValuesLeftEye.matHmdFromStage = m_hmdFromUniverse;
	shaderValuesLeftEye.camPos = glm::vec3( 1, 0, 0 );

	// right eye
	shaderValuesRightEye.matProjectionFromView = GetHMDMatrixProjectionEye( vr::Eye_Right );
	shaderValuesRightEye.matViewFromHmd = GetHMDMatrixPoseEye( vr::Eye_Right );
	shaderValuesRightEye.matHmdFromStage = m_hmdFromUniverse;
	shaderValuesRightEye.camPos = glm::vec3( 1, 0, 0 );
}

void VulkanExample::updateParams()
{
	shaderValuesParams.lightDir = glm::vec4(
		sin( glm::radians( lightSource.rotation.x ) ) * cos( glm::radians( lightSource.rotation.y ) ),
		sin( glm::radians( lightSource.rotation.y ) ),
		cos( glm::radians( lightSource.rotation.x ) ) * cos( glm::radians( lightSource.rotation.y ) ),
		0.0f );
	shaderValuesParams.debugViewInputs = 1;
}

void VulkanExample::windowResized()
{
	vkDeviceWaitIdle( device );
	updateUniformBuffers();
	updateOverlay();
}

void VulkanExample::prepare()
{
	VulkanExampleBase::prepare();

	camera.type = Camera::CameraType::lookat;

	camera.setPerspective( 45.0f, (float)width / (float)height, 0.1f, 256.0f );
	camera.rotationSpeed = 0.25f;
	camera.movementSpeed = 0.1f;
	camera.setPosition( { 0.0f, 0.0f, 1.0f } );
	camera.setRotation( { 0.0f, 0.0f, 0.0f } );

	waitFences.resize( renderAhead );
	presentCompleteSemaphores.resize( renderAhead );
	renderCompleteSemaphores.resize( renderAhead );
	commandBuffers.resize( swapChain.imageCount );
	uniformBuffers.resize( swapChain.imageCount );
	descriptorSets.resize( swapChain.imageCount );
	// Command buffer execution fences
	for ( auto &waitFence : waitFences ) {
		VkFenceCreateInfo fenceCI{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
		VK_CHECK_RESULT( vkCreateFence( device, &fenceCI, nullptr, &waitFence ) );
	}
	// Queue ordering semaphores
	for ( auto &semaphore : presentCompleteSemaphores ) {
		VkSemaphoreCreateInfo semaphoreCI{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0 };
		VK_CHECK_RESULT( vkCreateSemaphore( device, &semaphoreCI, nullptr, &semaphore ) );
	}
	for ( auto &semaphore : renderCompleteSemaphores ) {
		VkSemaphoreCreateInfo semaphoreCI{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0 };
		VK_CHECK_RESULT( vkCreateSemaphore( device, &semaphoreCI, nullptr, &semaphore ) );
	}
	// Command buffers
	{
		VkCommandBufferAllocateInfo cmdBufAllocateInfo{};
		cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmdBufAllocateInfo.commandPool = cmdPool;
		cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdBufAllocateInfo.commandBufferCount = static_cast<uint32_t>( commandBuffers.size() );
		VK_CHECK_RESULT( vkAllocateCommandBuffers( device, &cmdBufAllocateInfo, commandBuffers.data() ) );
	}

	vr::VRSystem()->GetRecommendedRenderTargetSize( &eyeWidth, &eyeHeight );

	leftEyeRT.init( swapChain.colorFormat, depthFormat, eyeWidth, eyeHeight, vulkanDevice, queue, settings.multiSampling );
	rightEyeRT.init( swapChain.colorFormat, depthFormat, eyeWidth, eyeHeight, vulkanDevice, queue, settings.multiSampling );

	loadAssets();
	generateBRDFLUT();
	generateCubemaps();
	prepareUniformBuffers();
	setupDescriptors();
	preparePipelines();

	ui = new UI( vulkanDevice, renderPass, queue, pipelineCache, settings.sampleCount );
	updateOverlay();

	m_pClient = kj::heap<aardvark::CAardvarkClient>();
	m_pClient->Start();

	m_frameListener = kj::heap<AvFrameListenerImpl>();
	m_frameListener->m_renderer = this;
	
	auto reqListen = m_pClient->Server().listenForFramesRequest();
	AvFrameListener::Client listenerClient = std::move( m_frameListener );
	reqListen.setListener( listenerClient );
	reqListen.send().wait( m_pClient->WaitScope() );

	vr::VRInput()->SetActionManifestPath( "e:/homedev/aardvark/data/input/aardvark_actions.json" );
	vr::VRInput()->GetActionSetHandle( "/actions/aardvark", &m_actionSet );
	vr::VRInput()->GetActionHandle( "/actions/aardvark/out/haptic", &m_actionHaptic );
	vr::VRInput()->GetActionHandle( "/actions/aardvark/in/grab", &m_actionGrab );
	vr::VRInput()->GetInputSourceHandle( "/user/hand/left", &m_leftHand );
	vr::VRInput()->GetInputSourceHandle( "/user/hand/right", &m_rightHand );

	prepared = true;
}

void VulkanExample::onWindowClose()
{
	if ( CAardvarkCefApp::instance() )
	{
		CAardvarkCefApp::instance()->CloseAllBrowsers( true );
	}
}

void VulkanExample::allBrowsersClosed()
{
	wantToQuit = true;
}




void VulkanExample::TraverseSceneGraphs( float fFrameTime )
{
	if ( !m_roots )
		return;

	inFrameTraversal = true;
	setVisitedNodes.clear();
	m_handDeviceForNode.clear();
	m_fThisFrameTime = fFrameTime;
	m_vecModelsToRender.clear();
	m_intersections.reset();
	m_collisions.reset();
	m_currentHandDevice = vr::k_ulInvalidInputValueHandle;
	m_currentGrabbableGlobalId = 0;
	m_nodeTransforms.clear();
	for ( auto & root : *m_roots )
	{
		TraverseSceneGraph( &*root );
	}
	m_pCurrentRoot = nullptr;

	m_lastFrameUniverseFromNode.clear();

	for ( auto & transform : m_nodeTransforms )
	{
		transform.second->resolve();
		m_lastFrameUniverseFromNode.insert_or_assign( transform.first, transform.second->getUniverseFromNode() );
	}

	inFrameTraversal = false;
}

uint64_t VulkanExample::GetGlobalId( const AvNode::Reader & node )
{
	assert( m_pCurrentRoot );
	if ( m_pCurrentRoot )
	{
		return ( (uint64_t)m_pCurrentRoot->gadgetId ) << 32 | node.getId();
	}
	else
	{
		return 0;
	}
}

VulkanExample::SgNodeData_t *VulkanExample::GetNodeData( const AvNode::Reader & node )
{
	// TODO(Joe): Figure out when to delete these
	uint64_t globalId = GetGlobalId( node );
	if ( !globalId )
		return nullptr;

	auto iData = m_mapNodeData.find( globalId );
	if ( iData != m_mapNodeData.end() )
	{
		return &*iData->second;
	}
	else
	{
		auto pData = std::make_unique<SgNodeData_t>();
		SgNodeData_t *pRetVal = &*pData;
		m_mapNodeData.insert( std::make_pair( globalId, std::move( pData ) ) );
		return pRetVal;
	}
}


void VulkanExample::TraverseSceneGraph( const SgRoot_t *root )
{
	if ( !root->nodes.empty() )
	{
		m_pCurrentRoot = root;

		// set the node 0 transform to its hook by default
		if ( !root->hook.empty() )
		{
			setHookOrigin( root->hook, root->nodes[0] );
		}

		// the 0th node is always the root
		TraverseNode( root->nodes[0], nullptr );
	}
}

void VulkanExample::TraverseNode( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	uint64_t globalId = GetGlobalId( node );
	if ( setVisitedNodes.find( globalId ) != setVisitedNodes.end() )
	{
		return;
	}
	setVisitedNodes.insert( globalId );

	vr::VRInputValueHandle_t handDeviceBefore = m_currentHandDevice;

	switch ( node.getType() )
	{
	case AvNode::Type::CONTAINER:
		// nothing special to do here
		break;

	case AvNode::Type::ORIGIN:
		TraverseOrigin( node, defaultParent );
		break;

	case AvNode::Type::TRANSFORM:
		TraverseTransform( node, defaultParent );
		break;

	case AvNode::Type::MODEL:
		TraverseModel( node, defaultParent );
		break;

	case AvNode::Type::PANEL:
		TraversePanel( node, defaultParent );
		break;

	case AvNode::Type::POKER:
		TraversePoker( node, defaultParent );
		break;

	case AvNode::Type::GRABBABLE:
		TraverseGrabbable( node, defaultParent );
		break;

	case AvNode::Type::HANDLE:
		TraverseHandle( node, defaultParent );
		break;

	case AvNode::Type::GRABBER:
		TraverseGrabber( node, defaultParent );
		break;

	case AvNode::Type::INVALID:
	default:
		assert( false );
	}

	uint64_t globalNodeId = GetGlobalId( node );
	CPendingTransform *thisNodeTransform = getTransform( globalNodeId );
	if ( thisNodeTransform->needsUpdate() )
	{
		thisNodeTransform->update( defaultParent, glm::mat4( 1.f ), nullptr );
	}

	m_handDeviceForNode.insert_or_assign( globalNodeId, m_currentHandDevice );

	for ( const uint32_t unChildId : node.getChildren() )
	{
		auto iChild = m_pCurrentRoot->mapIdToIndex.find( unChildId );
		if ( iChild != m_pCurrentRoot->mapIdToIndex.end() && iChild->second < m_pCurrentRoot->nodes.size() )
		{
			TraverseNode( m_pCurrentRoot->nodes[iChild->second], thisNodeTransform );
		}
	}

	if ( AvNode::Type::GRABBABLE == node.getType() )
	{
		m_currentGrabbableGlobalId = 0;
	}

	m_currentHandDevice = handDeviceBefore;
}

void VulkanExample::TraverseOrigin( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	std::string origin = node.getPropOrigin();
	setHookOrigin( origin, node );
}


void VulkanExample::setHookOrigin( std::string origin, const AvNode::Reader & node )
{
	auto iOrigin = m_universeFromOriginTransforms.find( origin );
	if ( iOrigin != m_universeFromOriginTransforms.end() )
	{
		updateTransform( GetGlobalId( node ), nullptr, iOrigin->second, nullptr );

		if ( origin == "/user/hand/left" )
		{
			m_currentHandDevice = m_leftHand;
		}
		else if ( origin == "/user/hand/right" )
		{
			m_currentHandDevice = m_rightHand;
		}
		else
		{
			m_currentHandDevice = vr::k_ulInvalidInputValueHandle;
		}
	}
}


void VulkanExample::TraverseTransform( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	if ( node.hasPropTransform() )
	{
		const AvTransform::Reader & transform = node.getPropTransform();
		glm::vec3 vTrans;
		if ( transform.hasPosition() )
		{
			vTrans.x = transform.getPosition().getX();
			vTrans.y = transform.getPosition().getY();
			vTrans.z = transform.getPosition().getZ();
		}
		else
		{
			vTrans.x = vTrans.y = vTrans.z = 0.f;
		}
		glm::vec3 vScale;
		if ( transform.hasScale() )
		{
			vScale.x = transform.getScale().getX();
			vScale.y = transform.getScale().getY();
			vScale.z = transform.getScale().getZ();
		}
		else
		{
			vScale.x = vScale.y = vScale.z = 1.f;
		}
		glm::quat qRot;
		if ( transform.hasRotation() )
		{
			qRot.x = transform.getRotation().getX();
			qRot.y = transform.getRotation().getY();
			qRot.z = transform.getRotation().getZ();
			qRot.w = transform.getRotation().getW();
		}
		else
		{
			qRot.x = qRot.y = qRot.z = 0.f;
			qRot.w = 1.f;
		}

		glm::mat4 matParentFromNode = glm::translate( glm::mat4( 1.0f ), vTrans ) * glm::mat4( qRot ) * glm::scale( glm::mat4( 1.0f ), vScale );
		updateTransform( GetGlobalId( node ), defaultParent, matParentFromNode, nullptr );
	}
}

void VulkanExample::TraverseModel( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	SgNodeData_t *pData = GetNodeData( node );
	assert( pData );

	std::string modelUri = node.getPropModelUri();
	if ( pData->lastModelUri != modelUri )
	{
		pData->model = nullptr;
	}

	if ( !pData->model )
	{
		auto model = findOrLoadModel( modelUri);
		if ( model )
		{
			pData->model = std::make_shared<vkglTF::Model>( *model );
			pData->model->parent = &pData->modelParent;
			pData->lastModelUri = modelUri;
		}
	}

	if ( pData->model )
	{
		m_vecModelsToRender.push_back( pData->model );

		updateTransform( GetGlobalId( node ), defaultParent, glm::mat4( 1.f ),
			[this, pData]( const glm::mat4 & universeFromNode )
		{
			pData->modelParent.matParentFromNode = universeFromNode;
			pData->model->animate( m_fThisFrameTime );

			// TODO(Joe): Figure out how to only do this when a parent is changing
			for ( auto &node : pData->model->nodes ) {
				node->update();
			}
		} );
	}
}

void VulkanExample::TraversePanel( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	SgNodeData_t *pData = GetNodeData( node );
	assert( pData );

	auto iSharedTexture = m_sharedTextureInfo->find( m_pCurrentRoot->gadgetId );

	if ( !pData->model && iSharedTexture != m_sharedTextureInfo->end() )
	{
		std::string sPanelModelUri = "file:///e:/homedev/aardvark/data/models/panel/panel.glb";
		if ( iSharedTexture->second.getInvertY() )
		{
			sPanelModelUri = "file:///e:/homedev/aardvark/data/models/panel/panel_inverted.glb";
		}

		auto model = findOrLoadModel( sPanelModelUri );
		if ( model )
		{
			pData->model = std::make_shared<vkglTF::Model>( *model );
			pData->model->parent = &pData->modelParent;
		}
	}

	if ( pData->model )
	{
		void *pvNewDxgiHandle = nullptr;
		uint32_t width = 0, height = 0;
		VkFormat textureFormat = VK_FORMAT_R8G8B8A8_UINT;
		VkFormat viewTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
		if ( iSharedTexture != m_sharedTextureInfo->end() )
		{
			pvNewDxgiHandle = reinterpret_cast<void*>( iSharedTexture->second.getSharedTextureHandle() );
			width = iSharedTexture->second.getWidth();
			height = iSharedTexture->second.getHeight();
			switch ( iSharedTexture->second.getFormat() )
			{
			default:
				assert( false );
				break;

			case AvSharedTextureInfo::Format::R8G8B8A8:
				textureFormat = VK_FORMAT_R8G8B8A8_UINT;
				viewTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
				break;

			case AvSharedTextureInfo::Format::B8G8R8A8:
				textureFormat = VK_FORMAT_B8G8R8A8_UINT;
				viewTextureFormat = VK_FORMAT_B8G8R8A8_UNORM;
				break;
			}
		}

		if ( pData->lastDxgiHandle != pvNewDxgiHandle )
		{
			pData->overrideTexture = std::make_shared<vks::Texture2D>();
			pData->overrideTexture->loadFromDxgiSharedHandle( pvNewDxgiHandle,
				textureFormat, viewTextureFormat,
				width, height,
				vulkanDevice, queue );

			for ( auto & material : pData->model->materials )
			{
				material.baseColorTexture = pData->overrideTexture;
			}

			setupDescriptorSetsForModel( pData->model );

			pData->lastDxgiHandle = pvNewDxgiHandle;
		}

		m_vecModelsToRender.push_back( pData->model );

		uint64_t globalId = GetGlobalId( node );
		updateTransform( globalId, defaultParent, glm::mat4( 1.f ),
			[this, pData, node, globalId ]( const glm::mat4 & universeFromNode )
		{
			pData->modelParent.matParentFromNode = universeFromNode;
			pData->model->animate( m_fThisFrameTime );

			// TODO(Joe): Figure out how to only do this when a parent is changing
			for ( auto &modelNode : pData->model->nodes ) {
				modelNode->update();
			}

			if ( node.getPropInteractive() )
			{
				glm::vec4 panelTangent = universeFromNode * glm::vec4( 0, 1.f, 0, 0 );
				float zScale = glm::length( panelTangent );
				m_intersections.addActivePanel(
					globalId,
					glm::inverse( universeFromNode ),
					zScale );
			}
		} );
	}

}

void VulkanExample::TraversePoker( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	uint64_t globalId = GetGlobalId( node );
	updateTransform( globalId, defaultParent, glm::mat4( 1.f ),
		[this, globalId ]( const glm::mat4 & universeFromNode )
	{
		glm::vec4 vPokerInUniverse = universeFromNode * glm::vec4( 0, 0, 0, 1.f );
		m_intersections.addActivePoker( globalId, vPokerInUniverse );
	} );
}

void VulkanExample::TraverseGrabbable( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	uint64_t globalId = GetGlobalId( node );
	m_currentGrabbableGlobalId = globalId;
	auto iParentTransform = m_nodeToNodeAnchors.find( globalId );
	if ( iParentTransform != m_nodeToNodeAnchors.end() )
	{
		// we have a parent from grabbing. Need to update to that.
		CPendingTransform *parent = getTransform( iParentTransform->second.parentNodeId );
		updateTransform( globalId, parent, iParentTransform->second.parentNodeFromThisNode, nullptr );
	}
}


void VulkanExample::TraverseHandle( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	if ( !node.hasPropVolume() )
	{
		return;
	}


	updateTransform( GetGlobalId( node ), defaultParent, glm::mat4( 1.f ),
		[this, node, grabbableId = m_currentGrabbableGlobalId ]( const glm::mat4 & universeFromNode )
	{
		m_collisions.addGrabbableHandle( grabbableId, universeFromNode, node.getPropVolume() );
	} );
}

void VulkanExample::TraverseGrabber( const AvNode::Reader & node, CPendingTransform *defaultParent )
{
	if ( !node.hasPropVolume() )
	{
		return;
	}

	uint64_t globalId = GetGlobalId( node );
	updateTransform( globalId, defaultParent, glm::mat4( 1.f ),
		[this, node, globalId, currentHandDevice = m_currentHandDevice ]( const glm::mat4 & universeFromNode )
	{
		m_collisions.addGrabber( globalId, glm::inverse( universeFromNode ),
			node.getPropVolume(), isGrabPressed( currentHandDevice ) );
	} );
}


void VulkanExample::applyFrame( AvVisualFrame::Reader & newFrame )
{
	camera.setPosition( { 0.0f, 0.0f, 1.0f } );
	camera.setRotation( { 0.0f, 0.0f, 0.0f } );

	auto nextRoots = std::make_unique < std::vector<std::unique_ptr<SgRoot_t>>>();
	for ( auto & root : newFrame.getRoots() )
	{
		std::unique_ptr<SgRoot_t> rootStruct = std::make_unique<SgRoot_t>();
		rootStruct->root = tools::newOwnCapnp( root );
		rootStruct->nodes.reserve( root.getNodes().size() );
		rootStruct->gadgetId = root.getSourceId();
		rootStruct->hook = root.getHook();

		for ( auto & nodeWrapper : rootStruct->root.getNodes() )
		{
			auto node = nodeWrapper.getNode();
			rootStruct->mapIdToIndex[node.getId()] = rootStruct->nodes.size();
			rootStruct->nodes.push_back( node );
		}

		nextRoots->push_back( std::move( rootStruct ) );
	}

	auto nextTextures = std::make_unique < std::map<uint32_t, tools::OwnCapnp< AvSharedTextureInfo > > >();
	for ( auto & texture : newFrame.getGadgetTextures() )
	{
		nextTextures->insert_or_assign( texture.getGadgetId(), tools::newOwnCapnp( texture.getSharedTextureInfo() ) );
	}

	m_nextRoots = std::move( nextRoots );
	m_nextSharedTextureInfo = std::move( nextTextures );
}

std::shared_ptr<vkglTF::Model> VulkanExample::findOrLoadModel( std::string modelUri )
{
	auto iModel = m_mapModels.find( modelUri );
	if ( iModel != m_mapModels.end() )
	{
		return iModel->second;
	}

	// below this point we're definitely going to return nullptr because we need to
	// make an async request for the model

	// if we've already failed, just return nullptr and don't keep trying
	if ( m_failedModelRequests.find( modelUri ) != m_failedModelRequests.end() )
		return nullptr;

	// if we already sent a request but are still waiting for the data, just return
	// null. The caller will call again next frame.
	if ( m_modelRequestsInProgress.find( modelUri ) != m_modelRequestsInProgress.end() )
		return nullptr;

	m_modelRequestsInProgress.insert( modelUri );

	auto reqModelSource = m_pClient->Server().getModelSourceRequest();
	reqModelSource.setUri( modelUri );
	auto promModelSource = reqModelSource.send()
		.then( [ this, modelUri ]( AvServer::GetModelSourceResults::Reader && res )
	{
		if ( res.getSuccess() )
		{
			// now get the actual data
			auto promModelData = res.getSource().dataRequest().send()
				.then( [this, modelUri]( AvModelSource::DataResults::Reader && res )
			{
				auto pModel = std::make_shared<vkglTF::Model>();
				bool bLoaded = pModel->loadFromMemory( &res.getData()[0], res.getData().size(), vulkanDevice, m_descriptorManager, queue );

				if ( bLoaded )
				{
					m_mapModels.insert( std::make_pair( modelUri, pModel ) );
					setupDescriptorSetsForModel( pModel );
					m_modelRequestsInProgress.erase( modelUri );
				}
				else
				{
					assert( bLoaded );
					m_failedModelRequests.insert( modelUri );
					m_modelRequestsInProgress.erase( modelUri );
				}
			} );

			m_pClient->addToTasks( std::move( promModelData ) );

		}
		else
		{
			m_failedModelRequests.insert( modelUri );
			m_modelRequestsInProgress.erase( modelUri );
		}
	} );
		
	m_pClient->addToTasks( std::move( promModelSource ) );

	return nullptr;
}

/*
	Update ImGui user interface
*/
void VulkanExample::updateOverlay()
{
	ImGuiIO& io = ImGui::GetIO();

	ImVec2 lastDisplaySize = io.DisplaySize;
	io.DisplaySize = ImVec2( (float)width, (float)height );
	io.DeltaTime = frameTimer;

	io.MousePos = ImVec2( mousePos.x, mousePos.y );
	io.MouseDown[0] = mouseButtons.left;
	io.MouseDown[1] = mouseButtons.right;

	ui->pushConstBlock.scale = glm::vec2( 2.0f / io.DisplaySize.x, 2.0f / io.DisplaySize.y );
	ui->pushConstBlock.translate = glm::vec2( -1.0f );

	bool updateShaderParams = false;
	float scale = 1.0f;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	scale = (float)vks::android::screenDensity / (float)ACONFIGURATION_DENSITY_MEDIUM;
#endif
	ImGui::NewFrame();

	ImGui::SetNextWindowPos( ImVec2( 10, 10 ) );
	ImGui::SetNextWindowSize( ImVec2( 200 * scale, 360 * scale ), ImGuiSetCond_Always );
	ImGui::Begin( "Aardvark Renderer", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove );
	ImGui::PushItemWidth( 100.0f * scale );

	ui->text( "Drawing frames..." );
	ui->text( "%.1d fps (%.2f ms)", lastFPS, ( 1000.0f / lastFPS ) );

	//		if (ui->header("Scene")) {
	//#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	//			if (ui->combo("File", selectedScene, scenes)) {
	//				vkDeviceWaitIdle(device);
	//				loadScene(scenes[selectedScene]);
	//				setupDescriptors();
	//				updateCBs = true;
	//			}
	//#else
	//			if (ui->button("Open gltf file")) {
	//				std::string filename = "";
	//#if defined(_WIN32)
	//				char buffer[MAX_PATH];
	//				OPENFILENAME ofn;
	//				ZeroMemory(&buffer, sizeof(buffer));
	//				ZeroMemory(&ofn, sizeof(ofn));
	//				ofn.lStructSize = sizeof(ofn);
	//				ofn.lpstrFilter = "glTF files\0*.gltf;*.glb\0";
	//				ofn.lpstrFile = buffer;
	//				ofn.nMaxFile = MAX_PATH;
	//				ofn.lpstrTitle = "Select a glTF file to load";
	//				ofn.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	//				if (GetOpenFileNameA(&ofn)) {
	//					filename = buffer;
	//				}
	//#elif defined(__linux__) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
	//				char buffer[1024];
	//				FILE *file = popen("zenity --title=\"Select a glTF file to load\" --file-filter=\"glTF files | *.gltf *.glb\" --file-selection", "r");
	//				if (file) {
	//					while (fgets(buffer, sizeof(buffer), file)) {
	//						filename += buffer;
	//					};
	//					filename.erase(std::remove(filename.begin(), filename.end(), '\n'), filename.end());
	//					std::cout << filename << std::endl;
	//				}
	//#endif
	//				if (!filename.empty()) {
	//					vkDeviceWaitIdle(device);
	//					loadScene(filename);
	//					setupDescriptors();
	//					updateCBs = true;
	//				}
	//			}
	//#endif
	//			if (ui->combo("Environment", selectedEnvironment, environments)) {
	//				vkDeviceWaitIdle(device);
	//				loadEnvironment(environments[selectedEnvironment]);
	//				setupDescriptors();
	//				updateCBs = true;
	//			}
	//		}
	//
	//		if (ui->header("Environment")) {
	//			if (ui->checkbox("Background", &displayBackground)) {
	//				updateShaderParams = true;
	//			}
	//			if (ui->slider("Exposure", &shaderValuesParams.exposure, 0.1f, 10.0f)) {
	//				updateShaderParams = true;
	//			}
	//			if (ui->slider("Gamma", &shaderValuesParams.gamma, 0.1f, 4.0f)) {
	//				updateShaderParams = true;
	//			}
	//			if (ui->slider("IBL", &shaderValuesParams.scaleIBLAmbient, 0.0f, 1.0f)) {
	//				updateShaderParams = true;
	//			}
	//		}
	//
	//		if (ui->header("Debug view")) {
	//			const std::vector<std::string> debugNamesInputs = {
	//				"none", "Base color", "Normal", "Occlusion", "Emissive", "Metallic", "Roughness"
	//			};
	//			if (ui->combo("Inputs", &debugViewInputs, debugNamesInputs)) {
	//				shaderValuesParams.debugViewInputs = (float)debugViewInputs;
	//				updateShaderParams = true;
	//			}
	//			const std::vector<std::string> debugNamesEquation = {
	//				"none", "Diff (l,n)", "F (l,h)", "G (l,v,h)", "D (h)", "Specular"
	//			};
	//			if (ui->combo("PBR equation", &debugViewEquation, debugNamesEquation)) {
	//				shaderValuesParams.debugViewEquation = (float)debugViewEquation;
	//				updateShaderParams = true;
	//			}
	//		}
	//
	//		if (models.scene.animations.size() > 0) {
	//			if (ui->header("Animations")) {
	//				ui->checkbox("Animate", &animate);
	//				std::vector<std::string> animationNames;
	//				for (auto animation : models.scene.animations) {
	//					animationNames.push_back(animation.name);
	//				}
	//				ui->combo("Animation", &animationIndex, animationNames);
	//			}
	//		}
	//
	ImGui::PopItemWidth();
	ImGui::End();
	ImGui::Render();

	ImDrawData* imDrawData = ImGui::GetDrawData();

	// Check if ui buffers need to be recreated
	if ( imDrawData ) {
		VkDeviceSize vertexBufferSize = imDrawData->TotalVtxCount * sizeof( ImDrawVert );
		VkDeviceSize indexBufferSize = imDrawData->TotalIdxCount * sizeof( ImDrawIdx );

		bool updateBuffers = ( ui->vertexBuffer.buffer == VK_NULL_HANDLE ) || ( ui->vertexBuffer.count != imDrawData->TotalVtxCount ) || ( ui->indexBuffer.buffer == VK_NULL_HANDLE ) || ( ui->indexBuffer.count != imDrawData->TotalIdxCount );

		if ( updateBuffers ) {
			vkDeviceWaitIdle( device );
			if ( ui->vertexBuffer.buffer ) {
				ui->vertexBuffer.destroy();
			}
			ui->vertexBuffer.create( vulkanDevice, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, vertexBufferSize );
			ui->vertexBuffer.count = imDrawData->TotalVtxCount;
			if ( ui->indexBuffer.buffer ) {
				ui->indexBuffer.destroy();
			}
			ui->indexBuffer.create( vulkanDevice, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, indexBufferSize );
			ui->indexBuffer.count = imDrawData->TotalIdxCount;
		}

		// Upload data
		ImDrawVert* vtxDst = (ImDrawVert*)ui->vertexBuffer.mapped;
		ImDrawIdx* idxDst = (ImDrawIdx*)ui->indexBuffer.mapped;
		for ( int n = 0; n < imDrawData->CmdListsCount; n++ ) {
			const ImDrawList* cmd_list = imDrawData->CmdLists[n];
			memcpy( vtxDst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof( ImDrawVert ) );
			memcpy( idxDst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof( ImDrawIdx ) );
			vtxDst += cmd_list->VtxBuffer.Size;
			idxDst += cmd_list->IdxBuffer.Size;
		}

		ui->vertexBuffer.flush();
		ui->indexBuffer.flush();

	}

	if ( updateShaderParams ) {
		updateParams();
	}

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	if ( mouseButtons.left ) {
		mouseButtons.left = false;
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Gets a Matrix Projection Eye with respect to nEye.
//-----------------------------------------------------------------------------
glm::mat4 VulkanExample::GetHMDMatrixProjectionEye( vr::Hmd_Eye nEye )
{
	if ( !vr::VRSystem() )
		return glm::mat4( 1.f );

	vr::HmdMatrix44_t mat = vr::VRSystem()->GetProjectionMatrix( nEye, 0.1f, 50.f );

	return glm::mat4(
		mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
		mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
		mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
		mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]
	);
}


//-----------------------------------------------------------------------------
// Purpose: Gets an HMDMatrixPoseEye with respect to nEye.
//-----------------------------------------------------------------------------
glm::mat4 VulkanExample::GetHMDMatrixPoseEye( vr::Hmd_Eye nEye )
{
	if ( !vr::VRSystem() )
		return glm::mat4( 1.f );

	vr::HmdMatrix34_t matEyeRight = vr::VRSystem()->GetEyeToHeadTransform( nEye );
	glm::mat4 matrixObj(
		matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
		matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
		matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
		matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f
	);

	return glm::inverse( matrixObj );
}

glm::mat4 VulkanExample::glmMatFromVrMat( const vr::HmdMatrix34_t & mat )
{
	//glm::mat4 r;
	glm::mat4 matrixObj(
		mat.m[0][0], mat.m[1][0], mat.m[2][0], 0.0,
		mat.m[0][1], mat.m[1][1], mat.m[2][1], 0.0,
		mat.m[0][2], mat.m[1][2], mat.m[2][2], 0.0,
		mat.m[0][3], mat.m[1][3], mat.m[2][3], 1.0f
	);
	//for ( uint32_t y = 0; y < 4; y++ )
	//{
	//	for ( uint32_t x = 0; x < 3; x++ )
	//	{
	//		r[x][y] = mat.m[x][y];
	//	}
	//	r[3][y] = y < 3 ? 0.f : 1.f;
	//}
	return matrixObj;
}

void VulkanExample::render()
{
	if ( !prepared )
	{
		// still pump the message loops
		m_pClient->WaitScope().poll();
		//			CefDoMessageLoopWork();

		return;
	}

	if ( m_nextRoots )
	{
		m_roots = std::move( m_nextRoots );
	}
	if ( m_nextSharedTextureInfo )
	{
		m_sharedTextureInfo = std::move( m_nextSharedTextureInfo );
	}

	updateOverlay();

	vr::TrackedDevicePose_t rRenderPoses[vr::k_unMaxTrackedDeviceCount];
	vr::TrackedDevicePose_t rGamePoses[vr::k_unMaxTrackedDeviceCount];
	vr::VRCompositor()->WaitGetPoses( rRenderPoses, vr::k_unMaxTrackedDeviceCount, rGamePoses, vr::k_unMaxTrackedDeviceCount );

	vr::TrackedDeviceIndex_t unLeftHand = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole( vr::TrackedControllerRole_LeftHand );
	if ( unLeftHand != vr::k_unTrackedDeviceIndexInvalid )
	{
		m_universeFromOriginTransforms["/user/hand/left"] = glmMatFromVrMat( rRenderPoses[unLeftHand].mDeviceToAbsoluteTracking );
	}
	vr::TrackedDeviceIndex_t unRightHand = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole( vr::TrackedControllerRole_RightHand );
	if ( unRightHand != vr::k_unTrackedDeviceIndexInvalid )
	{
		m_universeFromOriginTransforms["/user/hand/right"] = glmMatFromVrMat( rRenderPoses[unRightHand].mDeviceToAbsoluteTracking );
	}
	glm::mat4 universeFromHmd = glmMatFromVrMat( rRenderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking );
	m_hmdFromUniverse = glm::inverse( universeFromHmd );
	m_universeFromOriginTransforms["/user/head"] = universeFromHmd;
	m_universeFromOriginTransforms["/space/stage"] = glm::mat4( 1.f );

	TraverseSceneGraphs( frameTimer );

	if ( m_updateDescriptors )
	{
		m_descriptorManager->updateDescriptors();
		m_updateDescriptors = false;
	}

	VK_CHECK_RESULT( vkWaitForFences( device, 1, &waitFences[frameIndex], VK_TRUE, UINT64_MAX ) );
	VK_CHECK_RESULT( vkResetFences( device, 1, &waitFences[frameIndex] ) );

	VkResult acquire = swapChain.acquireNextImage( presentCompleteSemaphores[frameIndex], &currentBuffer );
	if ( ( acquire == VK_ERROR_OUT_OF_DATE_KHR ) || ( acquire == VK_SUBOPTIMAL_KHR ) ) {
		windowResize();
	}
	else {
		VK_CHECK_RESULT( acquire );
	}

	recordCommandBuffers( currentBuffer );

	// Update UBOs
	updateUniformBuffers();
	UniformBufferSet currentUB = uniformBuffers[currentBuffer];
	memcpy( currentUB.scene.mapped, &shaderValuesScene, sizeof( shaderValuesScene ) );
	memcpy( currentUB.leftEye.mapped, &shaderValuesLeftEye, sizeof( shaderValuesLeftEye ) );
	memcpy( currentUB.rightEye.mapped, &shaderValuesRightEye, sizeof( shaderValuesRightEye ) );
	memcpy( currentUB.params.mapped, &shaderValuesParams, sizeof( shaderValuesParams ) );
	memcpy( currentUB.skybox.mapped, &shaderValuesSkybox, sizeof( shaderValuesSkybox ) );

	const VkPipelineStageFlags waitDstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pWaitDstStageMask = &waitDstStageMask;
	submitInfo.pWaitSemaphores = &presentCompleteSemaphores[frameIndex];
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderCompleteSemaphores[frameIndex];
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pCommandBuffers = &commandBuffers[currentBuffer];
	submitInfo.commandBufferCount = 1;
	VK_CHECK_RESULT( vkQueueSubmit( queue, 1, &submitInfo, waitFences[frameIndex] ) );

	submitEyeBuffers();

	VkResult present = swapChain.queuePresent( queue, currentBuffer, renderCompleteSemaphores[frameIndex] );
	if ( !( ( present == VK_SUCCESS ) || ( present == VK_SUBOPTIMAL_KHR ) ) ) {
		if ( present == VK_ERROR_OUT_OF_DATE_KHR ) {
			windowResize();
			return;
		}
		else {
			VK_CHECK_RESULT( present );
		}
	}

	frameIndex += 1;
	frameIndex %= renderAhead;

	if ( !paused ) {
		if ( rotateModel ) {
			modelrot.y += frameTimer * 35.0f;
			if ( modelrot.y > 360.0f ) {
				modelrot.y -= 360.0f;
			}
		}

		updateParams();
		if ( rotateModel ) {
			updateUniformBuffers();
		}
	}
	if ( camera.updated ) {
		updateUniformBuffers();
	}

	m_intersections.updatePokerProximity( m_pClient );
	m_collisions.updateGrabberIntersections( m_pClient );

	doInputWork();

	// pump messages from RPC
	m_pClient->WaitScope().poll();

	// pump messages for CEF
	//CefDoMessageLoopWork();
}

bool GetAction( vr::VRActionHandle_t action, vr::VRInputValueHandle_t whichHand )
{
	vr::InputDigitalActionData_t actionData;
	vr::EVRInputError err = vr::VRInput()->GetDigitalActionData( action, &actionData,
		sizeof( actionData ), whichHand );
	if( vr::VRInputError_None != err )
		return false;

	return actionData.bActive && actionData.bState;
}


void VulkanExample::doInputWork()
{
	vr::VRActiveActionSet_t actionSet[2] = {};
	actionSet[0].ulActionSet = m_actionSet;
	actionSet[0].ulRestrictedToDevice = m_leftHand;
	actionSet[1].ulActionSet = m_actionSet;
	actionSet[1].ulRestrictedToDevice = m_rightHand;

	vr::EVRInputError err = vr::VRInput()->UpdateActionState( actionSet, sizeof( vr::VRActiveActionSet_t ), 2 );

	m_leftPressed = GetAction( m_actionGrab, m_leftHand );
	m_rightPressed = GetAction( m_actionGrab, m_rightHand );
}

bool VulkanExample::isGrabPressed( vr::VRInputValueHandle_t whichHand )
{
	if ( whichHand == m_leftHand )
	{
		return m_leftPressed;
	}
	else if ( whichHand == m_rightHand )
	{
		return m_rightPressed;
	}
	else
	{
		return false;
	}
}


void VulkanExample::sendHapticEvent( uint64_t targetGlobalNodeId, float amplitude, float frequency, float duration )
{
	auto iHapticDevice = m_handDeviceForNode.find( targetGlobalNodeId );
	if ( iHapticDevice == m_handDeviceForNode.end() )
	{
		return;
	}

	vr::VRInput()->TriggerHapticVibrationAction( 
		m_actionHaptic, 
		0, duration, frequency, amplitude, 
		iHapticDevice->second );
}


void VulkanExample::submitEyeBuffers()
{
	// Submit to OpenVR
	vr::VRTextureBounds_t bounds;
	bounds.uMin = 0.0f;
	bounds.uMax = 1.0f;
	bounds.vMin = 0.0f;
	bounds.vMax = 1.0f;

	vr::VRVulkanTextureData_t vulkanData;
	vulkanData.m_nImage = (uint64_t)leftEyeRT.color.image;
	vulkanData.m_pDevice = (VkDevice_T *)device;
	vulkanData.m_pPhysicalDevice = (VkPhysicalDevice_T *)vulkanDevice->physicalDevice;
	vulkanData.m_pInstance = (VkInstance_T *)instance;
	vulkanData.m_pQueue = (VkQueue_T *)queue;
	vulkanData.m_nQueueFamilyIndex = vulkanDevice->queueFamilyIndices.graphics;

	vulkanData.m_nWidth = eyeWidth;
	vulkanData.m_nHeight = eyeHeight;
	vulkanData.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM;
	vulkanData.m_nSampleCount = 1;

	vr::Texture_t texture = { &vulkanData, vr::TextureType_Vulkan, vr::ColorSpace_Auto };
	vr::VRCompositor()->Submit( vr::Eye_Left, &texture, &bounds );

	vulkanData.m_nImage = (uint64_t)rightEyeRT.color.image;
	vr::VRCompositor()->Submit( vr::Eye_Right, &texture, &bounds );

}

void VulkanExample::startGrabImpl( uint64_t grabberGlobalId, uint64_t grabbableGlobalId )
{
	auto iGrabbable = m_lastFrameUniverseFromNode.find( grabbableGlobalId );
	if ( iGrabbable == m_lastFrameUniverseFromNode.end() )
	{
		assert( false );
		return;
	}
	glm::mat4 universeFromGrabbable = iGrabbable->second;

	auto iGrabber = m_lastFrameUniverseFromNode.find( grabberGlobalId );
	if ( iGrabber == m_lastFrameUniverseFromNode.end() )
	{
		assert( false );
		return;
	}
	glm::mat4 grabberFromUniverse = glm::inverse( iGrabber->second );

	glm::mat4 grabberFromGrabbable = grabberFromUniverse * universeFromGrabbable;
	m_nodeToNodeAnchors.insert_or_assign( grabbableGlobalId, NodeToNodeAnchor_t{ grabberGlobalId, grabberFromGrabbable } );
}

void VulkanExample::endGrabImpl( uint64_t grabberGlobalId, uint64_t grabbableGlobalId )
{
	m_nodeToNodeAnchors.erase( grabbableGlobalId );
}

CPendingTransform *VulkanExample::getTransform( uint64_t globalNodeId )
{
	auto i = m_nodeTransforms.find( globalNodeId );
	if ( i == m_nodeTransforms.end() )
	{
		auto newTransform = m_nodeTransforms.insert_or_assign( globalNodeId, 
			std::make_unique<CPendingTransform>() );
		return newTransform.first->second.get();
	}
	else
	{
		return i->second.get();
	}
}

CPendingTransform *VulkanExample::updateTransform( uint64_t globalNodeId,
	CPendingTransform *parent, glm::mat4 parentFromNode, 
	std::function<void( const glm::mat4 & universeFromNode )> applyFunction )
{
	CPendingTransform *transform = getTransform( globalNodeId );
	transform->update( parent, parentFromNode, applyFunction );
	return transform;
}


::kj::Promise<void> AvFrameListenerImpl::newFrame( NewFrameContext context )
{
	m_renderer->applyFrame( context.getParams().getFrame() );
	return kj::READY_NOW;
}

::kj::Promise<void> AvFrameListenerImpl::sendHapticEvent( SendHapticEventContext context )
{
	m_renderer->sendHapticEvent( context.getParams().getTargetGlobalId(),
		context.getParams().getAmplitude(),
		context.getParams().getFrequency(),
		context.getParams().getDuration() );
	return kj::READY_NOW;
}

::kj::Promise<void> AvFrameListenerImpl::startGrab( StartGrabContext context )
{
	uint64_t grabberGlobalId = context.getParams().getGrabberGlobalId();
	uint64_t grabbableGlobalId = context.getParams().getGrabbableGlobalId();

	m_renderer->startGrabImpl( grabberGlobalId, grabbableGlobalId );
	return kj::READY_NOW;
}


::kj::Promise<void> AvFrameListenerImpl::endGrab( EndGrabContext context )
{
	uint64_t grabberGlobalId = context.getParams().getGrabberGlobalId();
	uint64_t grabbableGlobalId = context.getParams().getGrabbableGlobalId();
	m_renderer->endGrabImpl( grabberGlobalId, grabbableGlobalId );
	return kj::READY_NOW;
}

