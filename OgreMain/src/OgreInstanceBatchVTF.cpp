/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2009 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreStableHeaders.h"
#include "OgreInstanceBatchVTF.h"
#include "OgreSubMesh.h"
#include "OgreRenderOperation.h"
#include "OgreHardwareBufferManager.h"
#include "OgreHardwarePixelBuffer.h"
#include "OgreInstancedEntity.h"
#include "OgreMaterial.h"
#include "OgreTechnique.h"
#include "OgreMaterialManager.h"
#include "OgreTexture.h"
#include "OgreTextureManager.h"
#include "OgreRoot.h"

namespace Ogre
{
	static const uint16 c_maxTexWidth	= 4096;
	static const uint16 c_maxTexHeight	= 4096;

	InstanceBatchVTF::InstanceBatchVTF( InstanceManager *creator, MeshPtr &meshReference,
										const MaterialPtr &material, size_t instancesPerBatch,
										const Mesh::IndexMap *indexToBoneMap, const String &batchName ) :
				InstanceBatch( creator, meshReference, material, instancesPerBatch,
								indexToBoneMap, batchName ),
				m_numWorldMatrices( instancesPerBatch )
	{
		//We need to clone the material so we can have different textures for each batch.
		m_material = m_material->clone( mName + "/VTFMaterial" );
	}

	InstanceBatchVTF::~InstanceBatchVTF()
	{
		MaterialManager::getSingleton().remove( m_material->getName() );

		if( !m_matrixTexture.isNull() )
			TextureManager::getSingleton().remove( m_matrixTexture->getName() );
	}

	//-----------------------------------------------------------------------
	size_t InstanceBatchVTF::calculateMaxNumInstances( const SubMesh *baseSubMesh, uint16 flags ) const
	{
		size_t retVal = 0;

		RenderSystem *renderSystem = Root::getSingleton().getRenderSystem();
		const RenderSystemCapabilities *capabilities = renderSystem->getCapabilities();

		//VTF must be supported
		if( capabilities->hasCapability( RSC_VERTEX_TEXTURE_FETCH ) )
		{
			//TODO: Check PF_FLOAT32_RGBA is supported (should be, since it was the 1st one)
			const size_t numBones = std::max<size_t>( 1, baseSubMesh->blendIndexToBoneIndexMap.size() );
			retVal = c_maxTexWidth * c_maxTexHeight / 3 / numBones;

			if( flags & IM_USE16BIT )
			{
				if( baseSubMesh->vertexData->vertexCount * retVal > 0xFFFF )
					retVal = 0xFFFF / baseSubMesh->vertexData->vertexCount;
			}

			if( flags & IM_VTFBESTFIT )
			{
				const size_t instancesPerBatch = std::min( retVal, m_instancesPerBatch );
				//Do the same as in createVertexTexture()
				const size_t numBones = std::max<size_t>( 1, baseSubMesh->blendIndexToBoneIndexMap.size() );
				const size_t numWorldMatrices = instancesPerBatch * numBones;

				size_t texWidth  = std::min<size_t>( numWorldMatrices * 3, c_maxTexWidth );
				size_t texHeight = numWorldMatrices * 3 / c_maxTexWidth;
			
				const size_t remainder = (numWorldMatrices * 3) % c_maxTexWidth;

				if( remainder && texHeight > 0 )
					retVal = static_cast<size_t>(texWidth * texHeight / 3.0f / (float)(numBones));
			}
		}

		return retVal;
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::buildFrom( const SubMesh *baseSubMesh, const RenderOperation &renderOperation )
	{
		createVertexTexture( baseSubMesh );
		InstanceBatch::buildFrom( baseSubMesh, renderOperation );
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::setupVertices( const SubMesh* baseSubMesh )
	{
		m_renderOperation.vertexData = OGRE_NEW VertexData();

		VertexData *thisVertexData = m_renderOperation.vertexData;
		VertexData *baseVertexData = baseSubMesh->vertexData;

		thisVertexData->vertexStart = 0;
		thisVertexData->vertexCount = baseVertexData->vertexCount * m_instancesPerBatch;

		HardwareBufferManager::getSingleton().destroyVertexDeclaration( thisVertexData->vertexDeclaration );
		thisVertexData->vertexDeclaration = baseVertexData->vertexDeclaration->clone();

		HWBoneIdxVec hwBoneIdx;
		hwBoneIdx.resize( baseVertexData->vertexCount, 0 );
		if( m_meshReference->hasSkeleton() && !m_meshReference->getSkeleton().isNull() )
		{
			retrieveBoneIdx( baseVertexData, hwBoneIdx );

			thisVertexData->vertexDeclaration->removeElement( VES_BLEND_INDICES );
			thisVertexData->vertexDeclaration->removeElement( VES_BLEND_WEIGHTS );
			thisVertexData->vertexDeclaration->closeGapsInSource();
		}

		for( unsigned short i=0; i<thisVertexData->vertexDeclaration->getMaxSource()+1; ++i )
		{
			//Create our own vertex buffer
			HardwareVertexBufferSharedPtr vertexBuffer =
											HardwareBufferManager::getSingleton().createVertexBuffer(
											thisVertexData->vertexDeclaration->getVertexSize(i),
											thisVertexData->vertexCount,
											HardwareBuffer::HBU_STATIC_WRITE_ONLY );
			thisVertexData->vertexBufferBinding->setBinding( i, vertexBuffer );

			//Grab the base submesh data
			HardwareVertexBufferSharedPtr baseVertexBuffer =
													baseVertexData->vertexBufferBinding->getBuffer(i);

			char* thisBuf = static_cast<char*>(vertexBuffer->lock(HardwareBuffer::HBL_DISCARD));
			char* baseBuf = static_cast<char*>(baseVertexBuffer->lock(HardwareBuffer::HBL_READ_ONLY));

			//Copy and repeat
			for( size_t j=0; j<m_instancesPerBatch; ++j )
			{
				const size_t sizeOfBuffer = baseVertexData->vertexCount *
											baseVertexData->vertexDeclaration->getVertexSize(i);
				memcpy( thisBuf + j * sizeOfBuffer, baseBuf, sizeOfBuffer );
			}

			baseVertexBuffer->unlock();
			vertexBuffer->unlock();
		}

		createVertexTexture( baseSubMesh );
		createVertexSemantics( thisVertexData, baseVertexData, hwBoneIdx );
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::setupIndices( const SubMesh* baseSubMesh )
	{
		m_renderOperation.indexData = OGRE_NEW IndexData();

		IndexData *thisIndexData = m_renderOperation.indexData;
		IndexData *baseIndexData = baseSubMesh->indexData;

		thisIndexData->indexStart = 0;
		thisIndexData->indexCount = baseIndexData->indexCount * m_instancesPerBatch;

		//TODO: Check numVertices is below max supported by GPU
		HardwareIndexBuffer::IndexType indexType = HardwareIndexBuffer::IT_16BIT;
		if( m_renderOperation.vertexData->vertexCount > 65535 )
			indexType = HardwareIndexBuffer::IT_32BIT;
		thisIndexData->indexBuffer = HardwareBufferManager::getSingleton().createIndexBuffer(
													indexType, thisIndexData->indexCount,
													HardwareBuffer::HBU_STATIC_WRITE_ONLY );

		void *buf			= thisIndexData->indexBuffer->lock( HardwareBuffer::HBL_DISCARD );
		void const *baseBuf	= baseIndexData->indexBuffer->lock( HardwareBuffer::HBL_READ_ONLY );

		uint16 *thisBuf16 = static_cast<uint16*>(buf);
		uint32 *thisBuf32 = static_cast<uint32*>(buf);

		for( size_t i=0; i<m_instancesPerBatch; ++i )
		{
			const size_t vertexOffset = i * m_renderOperation.vertexData->vertexCount / m_instancesPerBatch;

			uint16 const *initBuf16 = static_cast<uint16 const *>(baseBuf);
			uint32 const *initBuf32 = static_cast<uint32 const *>(baseBuf);

			for( size_t j=0; j<baseIndexData->indexCount; ++j )
			{
				uint32 originalVal;
				if( baseSubMesh->indexData->indexBuffer->getType() == HardwareIndexBuffer::IT_16BIT )
					originalVal = *initBuf16++;
				else
					originalVal = *initBuf32++;

				if( indexType == HardwareIndexBuffer::IT_16BIT )
					*thisBuf16++ = static_cast<uint16>(originalVal) + vertexOffset;
				else
					*thisBuf32++ = originalVal + vertexOffset;
			}
		}

		baseIndexData->indexBuffer->unlock();
		thisIndexData->indexBuffer->unlock();
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::retrieveBoneIdx( VertexData *baseVertexData, HWBoneIdxVec &outBoneIdx )
	{
		const VertexElement *ve = baseVertexData->vertexDeclaration->
															findElementBySemantic( VES_BLEND_INDICES );
		const VertexElement *veWeights = baseVertexData->vertexDeclaration->
															findElementBySemantic( VES_BLEND_WEIGHTS );

		HardwareVertexBufferSharedPtr buff = baseVertexData->vertexBufferBinding->getBuffer(ve->getSource());
		char const *baseBuffer = static_cast<char const*>(buff->lock( HardwareBuffer::HBL_READ_ONLY ));

		for( size_t i=0; i<baseVertexData->vertexCount; ++i )
		{
			float const *pWeights = reinterpret_cast<float const*>(baseBuffer + veWeights->getOffset());

			uint8 biggestWeightIdx = 0;
			for( size_t j=1; j<veWeights->getSize() / 4; ++j )
				biggestWeightIdx = pWeights[biggestWeightIdx] < pWeights[j] ? j : biggestWeightIdx;

			uint8 const *pIndex = reinterpret_cast<uint8 const*>(baseBuffer + ve->getOffset());
			outBoneIdx[i] = pIndex[biggestWeightIdx];

			baseBuffer += baseVertexData->vertexDeclaration->getVertexSize(ve->getSource());
		}

		buff->unlock();
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::setupMaterialToUseVTF( TextureType textureType )
	{
		Material::TechniqueIterator techItor = m_material->getTechniqueIterator();
		while( techItor.hasMoreElements() )
		{
			Technique *technique = techItor.getNext();
			Technique::PassIterator passItor = technique->getPassIterator();

			while( passItor.hasMoreElements() )
			{
				bool bTexUnitFound = false;

				Pass *pass = passItor.getNext();
				Pass::TextureUnitStateIterator texUnitItor = pass->getTextureUnitStateIterator();

				while( texUnitItor.hasMoreElements() && !bTexUnitFound )
				{
					TextureUnitState *texUnit = texUnitItor.getNext();

					if( texUnit->getName() == "InstancingVTF" )
					{
						texUnit->setTextureName( m_matrixTexture->getName(), textureType );
						texUnit->setTextureFiltering( TFO_NONE );
						texUnit->setBindingType( TextureUnitState::BT_VERTEX );
					}
				}
			}
		}
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::createVertexTexture( const SubMesh* baseSubMesh )
	{
		/*
		TODO: Find a way to retrieve max texture resolution,
		http://www.ogre3d.org/forums/viewtopic.php?t=38305

		Currently assuming it's 4096x4096, which is a safe bet for any hardware with decent VTF*/

		const size_t numBones = std::max<size_t>( 1, baseSubMesh->blendIndexToBoneIndexMap.size() );
		m_numWorldMatrices = m_instancesPerBatch * numBones;

		//Calculate the width & height required to hold all the matrices. Start by filling the width
		//first (i.e. 4096x1 4096x2 4096x3, etc)
		size_t texWidth  = std::min<size_t>( m_numWorldMatrices * 3, c_maxTexWidth );
		size_t texHeight = m_numWorldMatrices * 3 / c_maxTexWidth;
		
		if( (m_numWorldMatrices * 3) % c_maxTexWidth )
			texHeight += 1;

		TextureType texType = texHeight == 1 ? TEX_TYPE_1D : TEX_TYPE_2D;

		m_matrixTexture = TextureManager::getSingleton().createManual(
										mName + "/VTF", m_meshReference->getGroup(), texType,
										(uint)texWidth, (uint)texHeight,
										0, PF_FLOAT32_RGBA, TU_DYNAMIC_WRITE_ONLY_DISCARDABLE );

		//Set our cloned material to use this custom texture!
		setupMaterialToUseVTF( texType );
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::createVertexSemantics( VertexData *thisVertexData, VertexData *baseVertexData,
													const HWBoneIdxVec &hwBoneIdx )
	{
		const size_t numBones = m_numWorldMatrices / m_instancesPerBatch;

		const size_t texWidth  = m_matrixTexture->getWidth();
		const size_t texHeight = m_matrixTexture->getHeight();

		//Calculate the texel offsets to correct them offline
		//Akwardly enough, the offset is needed in OpenGL too
		Vector2 texelOffsets;
		//RenderSystem *renderSystem = Root::getSingleton().getRenderSystem();
		texelOffsets.x = /*renderSystem->getHorizontalTexelOffset()*/ -0.5f / (float)texWidth;
		texelOffsets.y = /*renderSystem->getVerticalTexelOffset()*/ -0.5f / (float)texHeight;

		//Only one weight per vertex is supported. It would not only be complex, but prohibitively slow.
		//Put them in a new buffer, since it's 32 bytes aligned :-)
		const unsigned short newSource = thisVertexData->vertexDeclaration->getMaxSource() + 1;
		thisVertexData->vertexDeclaration->addElement( newSource, 0, VET_FLOAT4, VES_TEXTURE_COORDINATES,
														thisVertexData->vertexDeclaration->
																	getNextFreeTextureCoordinate() );
		thisVertexData->vertexDeclaration->addElement( newSource, 16, VET_FLOAT4, VES_TEXTURE_COORDINATES,
														thisVertexData->vertexDeclaration->
																	getNextFreeTextureCoordinate() );

		//Create our own vertex buffer
		HardwareVertexBufferSharedPtr vertexBuffer =
										HardwareBufferManager::getSingleton().createVertexBuffer(
										thisVertexData->vertexDeclaration->getVertexSize(newSource),
										thisVertexData->vertexCount,
										HardwareBuffer::HBU_STATIC_WRITE_ONLY );
		thisVertexData->vertexBufferBinding->setBinding( newSource, vertexBuffer );

		Vector2 *thisVec = static_cast<Vector2*>(vertexBuffer->lock(HardwareBuffer::HBL_DISCARD));

		//Copy and repeat
		for( size_t i=0; i<m_instancesPerBatch; ++i )
		{
			for( size_t j=0; j<baseVertexData->vertexCount; ++j )
			{
				for( size_t k=0; k<4; ++k )
				{
					size_t instanceIdx = (hwBoneIdx[j] + i * numBones) * 3 + k;
					thisVec->x = (instanceIdx % texWidth) / (float)texWidth;
					thisVec->y = (instanceIdx / texWidth) / (float)texHeight;
					*thisVec = *thisVec - texelOffsets;
					++thisVec;
				}
			}
		}

		vertexBuffer->unlock();
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::updateVertexTexture(void)
	{
		//Now lock the texture and copy the 4x3 matrices!
		m_matrixTexture->getBuffer()->lock( HardwareBuffer::HBL_DISCARD );
		const PixelBox &pixelBox = m_matrixTexture->getBuffer()->getCurrentLock();

		float *pDest = static_cast<float*>(pixelBox.data);

		InstancedEntityVec::const_iterator itor = m_instancedEntities.begin();
		InstancedEntityVec::const_iterator end  = m_instancedEntities.end();

		while( itor != end )
		{
			pDest += (*itor)->getTransforms3x4( pDest );
			++itor;
		}

		m_matrixTexture->getBuffer()->unlock();
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::getWorldTransforms( Matrix4* xform ) const
	{
		*xform = Matrix4::IDENTITY;
	}
	//-----------------------------------------------------------------------
	unsigned short InstanceBatchVTF::getNumWorldTransforms(void) const
	{
		return 1;
	}
	//-----------------------------------------------------------------------
	void InstanceBatchVTF::_updateRenderQueue(RenderQueue* queue)
	{
		const bool wasDirty = m_boundsDirty;

		InstanceBatch::_updateRenderQueue( queue );

		if( wasDirty || m_dirtyAnimation )
			updateVertexTexture();
	}
}