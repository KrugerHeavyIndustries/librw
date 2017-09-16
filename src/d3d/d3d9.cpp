#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d.h"
#include "rwd3d9.h"

#include "rwd3dimpl.h"

#define PLUGIN_ID 2

namespace rw {
namespace d3d9 {
using namespace d3d;

// TODO: move to header, but not as #define
#ifndef RW_D3D9
static VertexElement _d3ddec_end = {0xFF,0,D3DDECLTYPE_UNUSED,0,0,0};
#define D3DDECL_END() _d3ddec_end
#endif

#define NUMDECLELT 12

static void*
driverOpen(void *o, int32, int32)
{
	engine->driver[PLATFORM_D3D9]->defaultPipeline = makeDefaultPipeline();

	engine->driver[PLATFORM_D3D9]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_D3D9]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_D3D9]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_D3D9]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_D3D9]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_D3D9]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_D3D9]->rasterToImage      = rasterToImage;
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_D3D9, 0, PLATFORM_D3D9,
	                       driverOpen, driverClose);
	// shared between D3D8 and 9
	if(nativeRasterOffset == 0)
		registerNativeRaster();
}

void*
createVertexDeclaration(VertexElement *elements)
{
#ifdef RW_D3D9
	IDirect3DVertexDeclaration9 *decl = 0;
	d3ddevice->CreateVertexDeclaration((D3DVERTEXELEMENT9*)elements, &decl);
	return decl;
#else
	int n = 0;
	VertexElement *e = (VertexElement*)elements;
	while(e[n++].stream != 0xFF)
		;
	e = rwNewT(VertexElement, n, MEMDUR_EVENT | ID_DRIVER);
	memcpy(e, elements, n*sizeof(VertexElement));
	return e;
#endif
}

uint32
getDeclaration(void *declaration, VertexElement *elements)
{
#ifdef RW_D3D9
	IDirect3DVertexDeclaration9 *decl = (IDirect3DVertexDeclaration9*)declaration;
	UINT numElt;
	decl->GetDeclaration((D3DVERTEXELEMENT9*)elements, &numElt);
	return numElt;
#else
	int n = 0;
	VertexElement *e = (VertexElement*)declaration;
	while(e[n++].stream != 0xFF)
		;
	if(elements)
		memcpy(elements, declaration, n*sizeof(VertexElement));
	return n;
#endif
}


void*
destroyNativeData(void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D9)
		return object;
	InstanceDataHeader *header =
		(InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	deleteObject(header->vertexDeclaration);
	deleteObject(header->indexBuffer);
	deleteObject(header->vertexStream[0].vertexBuffer);
	deleteObject(header->vertexStream[1].vertexBuffer);
	rwFree(header->inst);
	rwFree(header);
	return object;
}

Stream*
readNativeData(Stream *stream, int32, void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_D3D9){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	geometry->instData = header;
	header->platform = PLATFORM_D3D9;

	int32 size = stream->readI32();
	uint8 *data = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_GEOMETRY);
	stream->read(data, size);
	uint8 *p = data;
	header->serialNumber = *(uint32*)p; p += 4;
	header->numMeshes = *(uint32*)p; p += 4;
	header->indexBuffer = nil; p += 4;
	header->primType = *(uint32*)p; p += 4;
	p += 16*2;	// skip vertex streams, they're repeated with the vertex buffers
	header->useOffsets = *(bool32*)p; p += 4;
	header->vertexDeclaration = nil; p += 4;
	header->totalNumIndex = *(uint32*)p; p += 4;
	header->totalNumVertex = *(uint32*)p; p += 4;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		inst->numIndex = *(uint32*)p; p += 4;
		inst->minVert = *(uint32*)p; p += 4;
		uint32 matid = *(uint32*)p; p += 4;
		inst->material = geometry->matList.materials[matid];
		inst->vertexAlpha = *(bool32*)p; p += 4;
		inst->vertexShader = nil; p += 4;
		inst->baseIndex = 0; p += 4;
		inst->numVertices = *(uint32*)p; p += 4;
		inst->startIndex = *(uint32*)p; p += 4;
		inst->numPrimitives = *(uint32*)p; p += 4;
		inst++;
	}

	VertexElement elements[NUMDECLELT];
	uint32 numDeclarations = stream->readU32();
	stream->read(elements, numDeclarations*8);
	header->vertexDeclaration = createVertexDeclaration(elements);

	header->indexBuffer = createIndexBuffer(header->totalNumIndex*2);
	uint16 *indices = lockIndices(header->indexBuffer, 0, 0, 0);
	stream->read(indices, 2*header->totalNumIndex);
	unlockIndices(header->indexBuffer);

	VertexStream *s;
	p = data;
	for(int i = 0; i < 2; i++){
		stream->read(p, 16);
		s = &header->vertexStream[i];
		s->vertexBuffer = (void*)(uintptr)*(uint32*)p; p += 4;
		s->offset = 0; p += 4;
		s->stride = *(uint32*)p; p += 4;
		s->geometryFlags = *(uint16*)p; p += 2;
		s->managed = *p++;
		s->dynamicLock = *p++;

		if(s->vertexBuffer == nil)
			continue;
		// TODO: unset managed flag when using morph targets.
		//       also uses different buffer type and locks differently
		s->vertexBuffer = createVertexBuffer(s->stride*header->totalNumVertex, 0, D3DPOOL_MANAGED);
		uint8 *verts = lockVertices(s->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
		stream->read(verts, s->stride*header->totalNumVertex);
		unlockVertices(s->vertexBuffer);
	}

	// TODO: somehow depends on number of streams used (baseIndex = minVert when more than one)
	inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		inst->baseIndex = inst->minVert + header->vertexStream[0].offset / header->vertexStream[0].stride;
		inst++;
	}

	rwFree(data);
	return stream;
}

Stream*
writeNativeData(Stream *stream, int32 len, void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	writeChunkHeader(stream, ID_STRUCT, len-12);
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D9)
		return stream;
	stream->writeU32(PLATFORM_D3D9);
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	int32 size = 64 + geometry->meshHeader->numMeshes*36;
	uint8 *data = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_GEOMETRY);
	stream->writeI32(size);

	uint8 *p = data;
	*(uint32*)p = header->serialNumber; p += 4;
	*(uint32*)p = header->numMeshes; p += 4;
	p += 4;		// skip index buffer
	*(uint32*)p = header->primType; p += 4;
	p += 16*2;	// skip vertex streams, they're repeated with the vertex buffers
	*(bool32*)p = header->useOffsets; p += 4;
	p += 4;		// skip vertex declaration
	*(uint32*)p = header->totalNumIndex; p += 4;
	*(uint32*)p = header->totalNumVertex; p += 4;

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		*(uint32*)p = inst->numIndex; p += 4;
		*(uint32*)p = inst->minVert; p += 4;
		int32 matid = geometry->matList.findIndex(inst->material);
		*(int32*)p = matid; p += 4;
		*(bool32*)p = inst->vertexAlpha; p += 4;
		*(uint32*)p = 0; p += 4;		// vertex shader
		*(uint32*)p = inst->baseIndex; p += 4;	// not used but meh...
		*(uint32*)p = inst->numVertices; p += 4;
		*(uint32*)p = inst->startIndex; p += 4;
		*(uint32*)p = inst->numPrimitives; p += 4;
		inst++;
	}
	stream->write(data, size);

	VertexElement elements[NUMDECLELT];
	uint32 numElt = getDeclaration(header->vertexDeclaration, elements);
	stream->writeU32(numElt);
	stream->write(elements, 8*numElt);

	uint16 *indices = lockIndices(header->indexBuffer, 0, 0, 0);
	stream->write(indices, 2*header->totalNumIndex);
	unlockIndices(header->indexBuffer);

	VertexStream *s;
	for(int i = 0; i < 2; i++){
		s = &header->vertexStream[i];
		p = data;
		*(uint32*)p = s->vertexBuffer ? 0xbadeaffe : 0; p += 4;
		*(uint32*)p = s->offset; p += 4;
		*(uint32*)p = s->stride; p += 4;
		*(uint16*)p = s->geometryFlags; p += 2;
		*p++ = s->managed;
		*p++ = s->dynamicLock;
		stream->write(data, 16);

		if(s->vertexBuffer == nil)
			continue;
		uint8 *verts = lockVertices(s->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
		stream->write(verts, s->stride*header->totalNumVertex);
		unlockVertices(s->vertexBuffer);
	}

	rwFree(data);
	return stream;
}

int32
getSizeNativeData(void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D9)
		return 0;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	int32 size = 12 + 4 + 4 + 64 + header->numMeshes*36;
	uint32 numElt = getDeclaration(header->vertexDeclaration, nil);
	size += 4 + numElt*8;
	size += 2*header->totalNumIndex;
	size += 0x10 + header->vertexStream[0].stride*header->totalNumVertex;
	size += 0x10 + header->vertexStream[1].stride*header->totalNumVertex;
	return size;
}

void
registerNativeDataPlugin(void)
{
	Geometry::registerPlugin(0, ID_NATIVEDATA,
	                         nil, destroyNativeData, nil);
	Geometry::registerPluginStream(ID_NATIVEDATA,
	                               readNativeData,
	                               writeNativeData,
	                               getSizeNativeData);
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// TODO: allow for REINSTANCE
	if(geo->instData)
		return;
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_D3D9;

	header->serialNumber = 0;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ? D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST;
	header->useOffsets = 0;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	header->indexBuffer = createIndexBuffer(header->totalNumIndex*2);

	uint16 *indices = lockIndices(header->indexBuffer, 0, 0, 0);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 startindex = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, (int32*)&inst->numVertices);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->vertexShader = nil;
		inst->baseIndex = inst->minVert;
		inst->startIndex = startindex;
		inst->numPrimitives = header->primType == D3DPT_TRIANGLESTRIP ? inst->numIndex-2 : inst->numIndex/3;
		if(inst->minVert == 0)
			memcpy(&indices[inst->startIndex], mesh->indices, inst->numIndex*2);
		else
			for(uint32 j = 0; j < inst->numIndex; j++)
				indices[inst->startIndex+j] = mesh->indices[j] - inst->minVert;
		startindex += inst->numIndex;
		mesh++;
		inst++;
	}
	unlockIndices(header->indexBuffer);

	memset(&header->vertexStream, 0, 2*sizeof(VertexStream));

	pipe->instanceCB(geo, header);
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if((geo->flags & Geometry::NATIVE) == 0)
		return;
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_D3D9);
	geo->numTriangles = geo->meshHeader->guessNumTriangles();
	geo->allocateData();
	geo->allocateMeshes(geo->meshHeader->numMeshes, geo->meshHeader->totalIndices, 0);

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	uint16 *indices = lockIndices(header->indexBuffer, 0, 0, 0);
	InstanceData *inst = header->inst;
	Mesh *mesh = geo->meshHeader->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++){
		if(inst->minVert == 0)
			memcpy(mesh->indices, &indices[inst->startIndex], inst->numIndex*2);
		else
			for(uint32 j = 0; j < inst->numIndex; j++)
				mesh->indices[j] = indices[inst->startIndex+j] + inst->minVert;
		mesh++;
		inst++;
	}
	unlockIndices(header->indexBuffer);

	pipe->uninstanceCB(geo, header);
	geo->generateTriangles();
	geo->flags &= ~Geometry::NATIVE;
	destroyNativeData(geo, 0, 0);
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// TODO: allow for REINSTANCE
	if(geo->instData == nil)
		pipe->instance(atomic);
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_D3D9);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

ObjPipeline::ObjPipeline(uint32 platform)
 : rw::ObjPipeline(platform)
{
	this->impl.instance = d3d9::instance;
	this->impl.uninstance = d3d9::uninstance;
	this->impl.render = d3d9::render;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	VertexElement dcl[NUMDECLELT];

	VertexStream *s = &header->vertexStream[0];
	s->offset = 0;
	s->managed = 1;
	s->geometryFlags = 0;
	s->dynamicLock = 0;

	int i = 0;
	dcl[i].stream = 0;
	dcl[i].offset = 0;
	dcl[i].type = D3DDECLTYPE_FLOAT3;
	dcl[i].method = D3DDECLMETHOD_DEFAULT;
	dcl[i].usage = D3DDECLUSAGE_POSITION;
	dcl[i].usageIndex = 0;
	i++;
	uint16 stride = 12;
	s->geometryFlags |= 0x2;

	bool isPrelit = (geo->flags & Geometry::PRELIT) != 0;
	if(isPrelit){
		dcl[i].stream = 0;
		dcl[i].offset = stride;
		dcl[i].type = D3DDECLTYPE_D3DCOLOR;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_COLOR;
		dcl[i].usageIndex = 0;
		i++;
		s->geometryFlags |= 0x8;
		stride += 4;
	}

	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		dcl[i].stream = 0;
		dcl[i].offset = stride;
		dcl[i].type = D3DDECLTYPE_FLOAT2;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_TEXCOORD;
		dcl[i].usageIndex = (uint8)n;
		i++;
		s->geometryFlags |= 0x10 << n;
		stride += 8;
	}

	bool hasNormals = (geo->flags & Geometry::NORMALS) != 0;
	if(hasNormals){
		dcl[i].stream = 0;
		dcl[i].offset = stride;
		dcl[i].type = D3DDECLTYPE_FLOAT3;
		dcl[i].method = D3DDECLMETHOD_DEFAULT;
		dcl[i].usage = D3DDECLUSAGE_NORMAL;
		dcl[i].usageIndex = 0;
		i++;
		s->geometryFlags |= 0x4;
		stride += 12;
	}
	dcl[i] = D3DDECL_END();
	header->vertexStream[0].stride = stride;

	header->vertexDeclaration = createVertexDeclaration((VertexElement*)dcl);

	s->vertexBuffer = createVertexBuffer(header->totalNumVertex*s->stride, 0, D3DPOOL_MANAGED);

	// TODO: support both vertex buffers
	uint8 *verts = lockVertices(s->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
	for(i = 0; dcl[i].usage != D3DDECLUSAGE_POSITION || dcl[i].usageIndex != 0; i++)
		;
	instV3d(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
		geo->morphTargets[0].vertices,
		header->totalNumVertex,
		header->vertexStream[dcl[i].stream].stride);

	if(isPrelit){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_COLOR || dcl[i].usageIndex != 0; i++)
			;
		// TODO: vertex alpha (instance per mesh)
		instColor(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			  geo->colors,
			  header->totalNumVertex,
			  header->vertexStream[dcl[i].stream].stride);
	}

	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_TEXCOORD || dcl[i].usageIndex != n; i++)
			;
		instTexCoords(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			geo->texCoords[n],
			header->totalNumVertex,
			header->vertexStream[dcl[i].stream].stride);
	}

	if(hasNormals){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_NORMAL || dcl[i].usageIndex != 0; i++)
			;
		instV3d(vertFormatMap[dcl[i].type], verts + dcl[i].offset,
			geo->morphTargets[0].normals,
			header->totalNumVertex,
			header->vertexStream[dcl[i].stream].stride);
	}
	unlockVertices(s->vertexBuffer);
}

void
defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	VertexElement dcl[NUMDECLELT];

	uint8 *verts[2];
	verts[0] = lockVertices(header->vertexStream[0].vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
	verts[1] = lockVertices(header->vertexStream[1].vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);

	int i;
	for(i = 0; dcl[i].usage != D3DDECLUSAGE_POSITION || dcl[i].usageIndex != 0; i++)
		;
	uninstV3d(vertFormatMap[dcl[i].type],
		  geo->morphTargets[0].vertices,
	          verts[dcl[i].stream] + dcl[i].offset,
		  header->totalNumVertex,
		  header->vertexStream[dcl[i].stream].stride);

	if(geo->flags & Geometry::PRELIT){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_COLOR || dcl[i].usageIndex != 0; i++)
			;
		uninstColor(vertFormatMap[dcl[i].type],
			    geo->colors,
		            verts[dcl[i].stream] + dcl[i].offset,
			    header->totalNumVertex,
			    header->vertexStream[dcl[i].stream].stride);
	}

	for(int32 n = 0; n < geo->numTexCoordSets; n++){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_TEXCOORD || dcl[i].usageIndex != n; i++)
			;
		uninstTexCoords(vertFormatMap[dcl[i].type],
			  geo->texCoords[n],
		          verts[dcl[i].stream] + dcl[i].offset,
			  header->totalNumVertex,
			  header->vertexStream[dcl[i].stream].stride);
	}

	if(geo->flags & Geometry::NORMALS){
		for(i = 0; dcl[i].usage != D3DDECLUSAGE_NORMAL || dcl[i].usageIndex != 0; i++)
			;
		uninstV3d(vertFormatMap[dcl[i].type],
			  geo->morphTargets[0].normals,
		          verts[dcl[i].stream] + dcl[i].offset,
			  header->totalNumVertex,
			  header->vertexStream[dcl[i].stream].stride);
	}

	unlockVertices(verts[0]);
	unlockVertices(verts[1]);
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = new ObjPipeline(PLATFORM_D3D9);
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

// Native Texture and Raster

Texture*
readNativeTexture(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_D3D9){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	// Texture
	tex->filterAddressing = stream->readU32();
	stream->read(tex->name, 32);
	stream->read(tex->mask, 32);

	// Raster
	int32 format = stream->readI32();
	int32 d3dformat = stream->readI32();
	int32 width = stream->readU16();
	int32 height = stream->readU16();
	int32 depth = stream->readU8();
	int32 numLevels = stream->readU8();
	int32 type = stream->readU8();
/*
#define HAS_ALPHA           (1<<0)
#define IS_CUBE             (1<<1)
#define USE_AUTOMIPMAPGEN   (1<<2)
#define IS_COMPRESSED       (1<<3)
*/
	int32 flags = stream->readU8();

	Raster *raster;
	D3dRaster *ext;

	if(flags & 8){
		// is compressed
		assert((flags & 2) == 0 && "Can't have cube maps yet");
		raster = Raster::create(width, height, depth, format | type | Raster::DONTALLOCATE, PLATFORM_D3D9);
		ext = PLUGINOFFSET(D3dRaster, raster, nativeRasterOffset);
		ext->format = d3dformat;
		ext->hasAlpha = flags & 1;
		ext->texture = createTexture(raster->width, raster->height,
		                             raster->format & Raster::MIPMAP ? numLevels : 1,
		                             ext->format);
		raster->flags &= ~Raster::DONTALLOCATE;
		ext->customFormat = 1;
	}else if(flags & 2){
		assert(0 && "Can't have cube maps yet");
	}else{
		raster = Raster::create(width, height, depth, format | type, PLATFORM_D3D9);
		ext = PLUGINOFFSET(D3dRaster, raster, nativeRasterOffset);
	}
	tex->raster = raster;

	// TODO: check if format supported and convert if necessary

	if(raster->format & Raster::PAL4)
		stream->read(ext->palette, 4*32);
	else if(raster->format & Raster::PAL8)
		stream->read(ext->palette, 4*256);

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		size = stream->readU32();
		if(i < raster->getNumLevels()){
			data = raster->lock(i);
			stream->read(data, size);
			raster->unlock(i);
		}else
			stream->seek(size);
	}
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_D3D9);

	// Texture
	stream->writeU32(tex->filterAddressing);
	stream->write(tex->name, 32);
	stream->write(tex->mask, 32);

	// Raster
	Raster *raster = tex->raster;
	D3dRaster *ext = PLUGINOFFSET(D3dRaster, raster, nativeRasterOffset);
	int32 numLevels = raster->getNumLevels();
	stream->writeI32(raster->format);
	stream->writeU32(ext->format);
	stream->writeU16(raster->width);
	stream->writeU16(raster->height);
	stream->writeU8(raster->depth);
	stream->writeU8(numLevels);
	stream->writeU8(raster->type);
	uint8 flags = 0;
	if(ext->hasAlpha)
		flags |= 1;
	// no automipmapgen and cube supported yet
	if(ext->customFormat)
		flags |= 8;
	stream->writeU8(flags);

	if(raster->format & Raster::PAL4)
		stream->write(ext->palette, 4*32);
	else if(raster->format & Raster::PAL8)
		stream->write(ext->palette, 4*256);

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		size = getLevelSize(raster, i);
		stream->writeU32(size);
		data = raster->lock(i);
		stream->write(data, size);
		raster->unlock(i);
	}
}

uint32
getSizeNativeTexture(Texture *tex)
{
	uint32 size = 12 + 72 + 16;
	int32 levels = tex->raster->getNumLevels();
	if(tex->raster->format & Raster::PAL4)
		size += 4*32;
	else if(tex->raster->format & Raster::PAL8)
		size += 4*256;
	for(int32 i = 0; i < levels; i++)
		size += 4 + getLevelSize(tex->raster, i);
	return size;
}

}
}
