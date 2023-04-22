#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <string.h>
#include <stdio.h>


#define _3DSGPU_CPP_
#include "snes9x.h"
#include "memmap.h"
#include "3dsgpu.h"
#include "3dsfiles.h"
#include "3dsimpl.h"
#include "3dssettings.h"

#ifndef M_PI
#define	M_PI		3.14159265358979323846
#endif

bool somethingWasDrawn = false;
bool somethingWasFlushed = false;

extern "C" u32 __ctru_linear_heap;
extern "C" u32 __ctru_linear_heap_size;

/*
For reference only:

GSPGPU_FramebufferFormat {
  GSP_RGBA8_OES =0,
  GSP_BGR8_OES =1,
  GSP_RGB565_OES =2,
  GSP_RGB5_A1_OES =3,
  GSP_RGBA4_OES =4
}

GPU_TEXCOLOR {
  GPU_RGBA8 = 0x0,
  GPU_RGB8 = 0x1,
  GPU_RGBA5551 = 0x2,
  GPU_RGB565 = 0x3,
  GPU_RGBA4 = 0x4,
  GPU_LA8 = 0x5,
  GPU_HILO8 = 0x6,
  GPU_L8 = 0x7,
  GPU_A8 = 0x8,
  GPU_LA4 = 0x9,
  GPU_L4 = 0xA,
  GPU_ETC1 = 0xB,
  GPU_ETC1A4 = 0xC
}

GX_TRANSFER_FORMAT {
  GX_TRANSFER_FMT_RGBA8 = 0,
  GX_TRANSFER_FMT_RGB8 = 1,
  GX_TRANSFER_FMT_RGB565 = 2,
  GX_TRANSFER_FMT_RGB5A1 = 3,
  GX_TRANSFER_FMT_RGBA4 = 4
}
*/

#define LINEARFREE_SAFE(x)  if (x) linearFree(x);


//------------------------------------------------------------------------
// Increased buffer size to 1MB for screens with heavy effects (multiple wavy backgrounds and line-by-line windows).
// Memory Usage = 2.00 MB   for GPU command buffer
#define COMMAND_BUFFER_SIZE             0x200000




u32 *gpuCommandBuffer1;
u32 *gpuCommandBuffer2;
int gpuCommandBufferSize = 0;
int gpuCurrentCommandBuffer = 0;
SGPU3DS GPU3DS;

int renderTargetVertexShaderRegister = 0;
int renderTargetGeometryShaderRegister = 0;
int textureVertexShaderRegister = 0;
int textureGeometryShaderRegister = 0;
int textureOffsetVertexShaderRegister = 0;

u32 vertexListBufferOffsets[1] = { 0 };
u64 vertexListAttribPermutations[1] = { 0x3210 };
u8 vertexListNumberOfAttribs[1] = { 2 };

inline void gpu3dsSetAttributeBuffers(
    u8 totalAttributes,
    u32 *listAddress, u64 attributeFormats)
{
    if (GPU3DS.currentAttributeBuffer != listAddress)
    {
        u32 *osAddress = (u32 *)osConvertVirtToPhys(listAddress);

        // Some very minor optimizations
        if (GPU3DS.currentTotalAttributes != totalAttributes ||
            GPU3DS.currentAttributeFormats != attributeFormats)
        {
            vertexListNumberOfAttribs[0] = totalAttributes;
            GPU_SetAttributeBuffers(
                totalAttributes, // number of attributes
                osAddress,
                attributeFormats,
                0xFFFF, //0b1100
                0x3210,
                1, //number of buffers
                vertexListBufferOffsets,        // buffer offsets (placeholders)
                vertexListAttribPermutations,   // attribute permutations for each buffer
                vertexListNumberOfAttribs       // number of attributes for each buffer
            );
            GPU3DS.currentTotalAttributes = totalAttributes;
            GPU3DS.currentAttributeFormats = attributeFormats;
        }
        else
        {
            GPUCMD_AddWrite(GPUREG_ATTRIBBUFFERS_LOC, ((u32)osAddress)>>3);

            // The real 3DS doesn't allow us to set the osAddress independently without
            // setting the additional register as below. If we don't do this, the
            // 3DS GPU will freeze up.
            //
            GPUCMD_AddMaskedWrite(GPUREG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000|(totalAttributes-1));
        }

        GPU3DS.currentAttributeBuffer = listAddress;
    }

}

//---------------------------------------------------------
// Enables / disables the parallax barrier
// Taken from RetroArch
//---------------------------------------------------------
void gpu3dsSetParallaxBarrier(bool enable)
{
   u32 reg_state = enable ? 0x00010001: 0x0;
   GSPGPU_WriteHWRegs(0x202000, &reg_state, 4);
}


//---------------------------------------------------------
// Sets the 2D screen mode based on the 3D slider.
// Taken from RetroArch.
//---------------------------------------------------------
float prevSliderVal = -1;
void gpu3dsCheckSlider()
{
    float sliderVal = *(float*)0x1FF81080;

    if (sliderVal != prevSliderVal)
    {
        if (sliderVal < 0.6)
        {
            gpu3dsSetParallaxBarrier(false);
        }
        else
        {
            gpu3dsSetParallaxBarrier(true);
        }

        gfxScreenSwapBuffers(GFX_TOP, false);
    }
    prevSliderVal = sliderVal;
}

void gpu3dsEnableDepthTest()
{
	GPU_SetDepthTestAndWriteMask(true, GPU_GEQUAL, GPU_WRITE_ALL);
}

void gpu3dsDisableDepthTest()
{
	GPU_SetDepthTestAndWriteMask(false, GPU_ALWAYS, GPU_WRITE_ALL);
}


void gpu3dsEnableStencilTest(GPU_TESTFUNC func, u8 ref, u8 input_mask)
{
    GPU_SetStencilTest(true, func, ref, input_mask, 0);
}

void gpu3dsDisableStencilTest()
{
	GPU_SetStencilTest(false, GPU_ALWAYS, 0x00, 0x00, 0x00);
}


void gpu3dsClearTextureEnv(u8 num)
{
	GPU_SetTexEnv(num,
		GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0),
		GPU_TEVSOURCES(GPU_PREVIOUS, 0, 0),
		GPU_TEVOPERANDS(0,0,0),
		GPU_TEVOPERANDS(0,0,0),
		GPU_REPLACE,
		GPU_REPLACE,
		0x80808080);
}

void gpu3dsSetTextureEnvironmentReplaceColor()
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR),
		GPU_TEVSOURCES(GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_REPLACE,
		0x80808080
	);

	gpu3dsClearTextureEnv(1);
}

void gpu3dsSetTextureEnvironmentReplaceColorButKeepAlpha()
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR),
		GPU_TEVSOURCES(GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_REPLACE,
		0x80808080
	);

	gpu3dsClearTextureEnv(1);
}

void gpu3dsSetTextureEnvironmentReplaceTexture0()
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_REPLACE,
		0x80808080
	);

	gpu3dsClearTextureEnv(1);
}

void gpu3dsSetTextureEnvironmentReplaceTexture0WithColorAlpha()
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_MODULATE,
		0x80808080
	);

	gpu3dsClearTextureEnv(1);
}

void gpu3dsSetTextureEnvironmentReplaceTexture0WithFullAlpha()
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
		GPU_TEVSOURCES(GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_REPLACE,
		0xffffffff
	);

	gpu3dsClearTextureEnv(1);
}

void gpu3dsSetTextureEnvironmentReplaceTexture0WithConstantAlpha(uint8 alpha)
{
	GPU_SetTexEnv(
		0,
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
		GPU_TEVSOURCES(GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT ),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_TEVOPERANDS(0, 0, 0),
		GPU_REPLACE, GPU_MODULATE,
		0x00808000 | alpha | (alpha << 24)
	);

	gpu3dsClearTextureEnv(1);
}


void *gpu3dsAlignTo0x80 (void *addr)
{
    if ((u32)addr & 0x7f)
        return (void *)(((u32)addr & ~0x7f) + 0x80);
    return addr;
}


void gpu3dsAllocVertexList(SVertexList *list, int sizeInBytes, int vertexSize,
    u8 totalAttributes, u64 attributeFormats)
{
    list->TotalAttributes = totalAttributes;
    list->AttributeFormats = attributeFormats;
    list->VertexSize = vertexSize;
    list->SizeInBytes = sizeInBytes;
    list->ListBase = (STileVertex *) linearAlloc(sizeInBytes);
    list->List = list->ListBase;
    list->ListOriginal = list->List;
    list->Total = 0;
    list->Count = 0;
    list->Flip = 1;
}

void gpu3dsDeallocVertexList(SVertexList *list)
{
    LINEARFREE_SAFE(list->ListBase);
}

void gpu3dsSwapVertexListForNextFrame(SVertexList *list)
{
    if (list->Flip)
        list->List = (void *)((uint32)(list->ListBase) + list->SizeInBytes / 2);
    else
        list->List = list->ListBase;
    list->ListOriginal = list->List;
    list->Flip = 1 - list->Flip;
    list->Total = 0;
    list->Count = 0;
    list->FirstIndex = 0;
    list->PrevCount = 0;
    list->PrevFirstIndex = 0;
}


void gpu3dsDrawVertexList(SVertexList *list, GPU_Primitive_t type, bool repeatLastDraw, int storeVertexListIndex, int storeIndex)
{
    if (!repeatLastDraw)
    {
        if (list->Count > 0)
        {
            //printf ("  DVL         : %8x count=%d\n", list->List, list->Count);
            gpu3dsSetAttributeBuffers(
                list->TotalAttributes,          // number of attributes
                (u32*)list->List,
                list->AttributeFormats
            );

            GPU_DrawArray(type, 0, list->Count);

            // Save the parameters passed to the gpu3dsSetAttributeBuffers and GPU_DrawArray
            //
            if (storeVertexListIndex >= 0 && storeIndex >= 0)
            {
                GPU3DS.vertexesStored[storeVertexListIndex][storeIndex].TotalAttributes = list->TotalAttributes;
                GPU3DS.vertexesStored[storeVertexListIndex][storeIndex].List = list->List;
                GPU3DS.vertexesStored[storeVertexListIndex][storeIndex].AttributeFormats = list->AttributeFormats;
                GPU3DS.vertexesStored[storeVertexListIndex][storeIndex].Count = list->Count;
            }

            // Saves this just in case it can be re-used for windowing
            // or HDMA effects.
            //
            list->PrevCount = list->Count;
            list->PrevFirstIndex = list->FirstIndex;
            list->PrevList = list->List;

            u8 *p = (u8 *)list->List;
            list->List = (STileVertex *) gpu3dsAlignTo0x80(p + (list->Count * list->VertexSize));

            list->FirstIndex += list->Count;
            list->Total += list->Count;
            list->Count = 0;

            somethingWasDrawn = true;
        }
        else
        {
            // Save the parameters passed to the gpu3dsSetAttributeBuffers and GPU_DrawArray
            //
            if (storeVertexListIndex >= 0 && storeIndex >= 0)
            {
                GPU3DS.vertexesStored[storeVertexListIndex][storeIndex].Count = list->Count;
            }

        }
    }
    else
    {
        SStoredVertexList *list = &GPU3DS.vertexesStored[storeVertexListIndex][storeIndex];
        if (list->Count > 0)
        {
            //printf ("  DVL (repeat): %8x count=%d\n", list->List, list->Count);
            gpu3dsSetAttributeBuffers(
                list->TotalAttributes,          // number of attributes
                (u32*)list->List,
                list->AttributeFormats
            );

            GPU_DrawArray(type, 0, list->Count);

            somethingWasDrawn = true;
        }
    }
}


void gpu3dsDrawVertexList(SVertexList *list, GPU_Primitive_t type, int fromIndex, int tileCount)
{
    if (tileCount > 0)
    {
        gpu3dsSetAttributeBuffers(
            list->TotalAttributes,          // number of attributes
            (u32 *)list->List,
            list->AttributeFormats
        );


        if (GPU3DS.isReal3DS)
            GPU_DrawArray(type, fromIndex, tileCount);
        else
            GPU_DrawArray(type, fromIndex * 6, tileCount * 6);

        somethingWasDrawn = true;
    }
}




bool gpu3dsInitialize()
{

    // Initialize the 3DS screen
    //
    GPU3DS.screenFormat = GSP_RGBA8_OES;
    gfxInit(GPU3DS.screenFormat, GPU3DS.screenFormat, false);
	gfxSet3D(false);
    gfxSetWide(true);
    APT_CheckNew3DS(&GPU3DS.isNew3DS);

    // Create the frame and depth buffers for the main screen.
    //
    GPU3DS.frameBufferFormat = GPU_RGBA8;
	GPU3DS.frameBuffer = (u32 *) vramMemAlign(SCREEN_TOP_WIDTH * SCREEN_HEIGHT * 8, 0x100);
	GPU3DS.frameDepthBuffer = (u32 *) vramMemAlign(SCREEN_TOP_WIDTH * SCREEN_HEIGHT * 8, 0x100);
    if (GPU3DS.frameBuffer == NULL ||
        GPU3DS.frameDepthBuffer == NULL)
    {
        printf ("Unable to allocate frame/depth buffers\n");
        return false;
    }

    // Initialize the sub screen for console output.
    //
    consoleInit(screenSettings.SecondScreen, NULL);

    // Create the command buffers
    //
    gpuCommandBufferSize = COMMAND_BUFFER_SIZE;
    gpuCommandBuffer1 = (u32 *)linearAlloc(COMMAND_BUFFER_SIZE / 2);
    gpuCommandBuffer2 = (u32 *)linearAlloc(COMMAND_BUFFER_SIZE / 2);
    if (gpuCommandBuffer1 == NULL || gpuCommandBuffer2 == NULL)
        return false;
	GPUCMD_SetBuffer(gpuCommandBuffer1, gpuCommandBufferSize, 0);
    gpuCurrentCommandBuffer = 0;

#ifndef RELEASE
    printf ("Buffer: %8x\n", (u32) gpuCommandBuffer1);
#endif

#ifdef RELEASE
    GPU3DS.isReal3DS = true;
#else
    if (file3dsGetCurrentDir()[0] != '/')
        GPU3DS.isReal3DS = true;
    else
        GPU3DS.isReal3DS = false;
#endif

    // Initialize the projection matrix for the top / bottom
    // screens
    //
	matrix3dsInitOrthographic(GPU3DS.projectionTopScreen,
        0.0f, 800.0f, 0.0f, 240.0f, 0.0f, 1.0f);
	matrix3dsInitOrthographic(GPU3DS.projectionBottomScreen,
        0.0f, 320.0f, 0.0f, 240.0f, 0.0f, 1.0f);

    // Initialize all shaders to empty
    //
    for (int i = 0; i < 10; i++)
    {
        GPU3DS.shaders[i].dvlb = NULL;
    }

    // Initialize texture offsets for hi-res
    //
    gpu3dsSetTextureOffset(0, 0);

#ifndef RELEASE
    printf ("gpu3dsInitialize - Allocate buffers\n");
#endif

#ifndef RELEASE
    printf ("gpu3dsInitialize - Set GPU statuses\n");
#endif

	GPU_DepthMap(-1.0f, 0.0f);
	GPU_SetDepthTestAndWriteMask(false, GPU_GEQUAL, GPU_WRITE_ALL);

	GPUCMD_AddMaskedWrite(GPUREG_EARLYDEPTH_TEST1, 0x1, 0);
	GPUCMD_AddWrite(GPUREG_EARLYDEPTH_TEST2, 0);
	GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, GPU_CULL_NONE&0x3);
    
	GPU_SetStencilTest(false, GPU_ALWAYS, 0x00, 0xFF, 0x00);
	GPU_SetStencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);

	GPU_SetBlendingColor(0,0,0,0);
	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ONE, GPU_ZERO
	);
	gpu3dsEnableAlphaTestNotEqualsZero();
    GPUCMD_AddWrite(GPUREG_TEXUNIT0_BORDER_COLOR, 0);
    gpu3dsSetTextureEnvironmentReplaceTexture0();
    gpu3dsFlush();
    gpu3dsWaitForPreviousFlush();

    return true;
}


void gpu3dsFinalize()
{
    // Bug fix: Free up all shaders' DVLB
    //
    // Initialize all shaders to empty
    //
    for (int i = 0; i < 10; i++)
    {
        if (GPU3DS.shaders[i].dvlb)
            DVLB_Free(GPU3DS.shaders[i].dvlb);
    }

    // Bug fix: free the frame buffers!
    if (GPU3DS.frameBuffer) vramFree(GPU3DS.frameBuffer);
    if (GPU3DS.frameDepthBuffer) vramFree(GPU3DS.frameDepthBuffer);

    LINEARFREE_SAFE(gpuCommandBuffer1);
    LINEARFREE_SAFE(gpuCommandBuffer2);

#ifndef RELEASE
    printf("gfxExit:\n");
#endif

	gfxExit();
}

void gpu3dsEnableAlphaTestNotEqualsZero()
{
    GPU_SetAlphaTest(true, GPU_NOTEQUAL, 0x00);
}

void gpu3dsEnableAlphaTestEqualsOne()
{
    GPU_SetAlphaTest(true, GPU_EQUAL, 0x01);
}

void gpu3dsEnableAlphaTestEquals(uint8 alpha)
{
    GPU_SetAlphaTest(true, GPU_EQUAL, alpha);
}

void gpu3dsEnableAlphaTestGreaterThanEquals(uint8 alpha)
{
    GPU_SetAlphaTest(true, GPU_GEQUAL, alpha);
}


void gpu3dsDisableAlphaTest()
{
    GPU_SetAlphaTest(false, GPU_NOTEQUAL, 0x00);
}



int gpu3dsGetPixelSize(GPU_TEXCOLOR pixelFormat)
{
    if (pixelFormat == GPU_RGBA8)
        return 4;
    if (pixelFormat == GPU_RGB8)
        return 3;
    if (pixelFormat == GPU_RGBA5551)
        return 2;
    if (pixelFormat == GPU_RGB565)
        return 2;
    if (pixelFormat == GPU_RGBA4)
        return 2;
    return 0;
}


SGPUTexture *gpu3dsCreateTextureInLinearMemory(int width, int height, GPU_TEXCOLOR pixelFormat)
{
	int size = width * height * gpu3dsGetPixelSize(pixelFormat);
    if (size == 0)
        return NULL;

	void *data = linearMemAlign(size, 0x80);

	SGPUTexture *texture = (SGPUTexture *) malloc(sizeof(SGPUTexture));

	texture->Memory = 0;
	texture->PixelFormat = pixelFormat;
	texture->Params = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST)
		| GPU_TEXTURE_MIN_FILTER(GPU_NEAREST)
		| GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER)
		| GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER);
	texture->Width = width;
	texture->Height = height;
	texture->PixelData = data;
    texture->BufferSize = size;
    texture->TextureScale[3] = 1.0f / texture->Width;  // x
    texture->TextureScale[2] = 1.0f / texture->Height; // y
    texture->TextureScale[1] = 0;  // z
    texture->TextureScale[0] = 0;  // w

    memset(texture->PixelData, 0, size);

#ifndef RELEASE
    printf ("Allocated %d x %d in linear mem (%d)\n", width, height, size);
#endif

	return texture;
}

void gpu3dsDestroyTextureFromLinearMemory(SGPUTexture *texture)
{
    LINEARFREE_SAFE(texture->PixelData);
    if (texture) free(texture);
}

SGPUTexture *gpu3dsCreateTextureInVRAM(int width, int height, GPU_TEXCOLOR pixelFormat)
{
	int size = width * height * gpu3dsGetPixelSize(pixelFormat);
    if (size == 0)
        return NULL;

	void *data = vramMemAlign(size, 0x80);

	SGPUTexture *texture = (SGPUTexture *) malloc(sizeof(SGPUTexture));

	texture->Memory = 1;
	texture->PixelFormat = pixelFormat;
	texture->Params = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST)
		| GPU_TEXTURE_MIN_FILTER(GPU_NEAREST)
		| GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER)
		| GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER);
	texture->Width = width;
	texture->Height = height;
	texture->PixelData = data;
    texture->BufferSize = size;
    texture->TextureScale[3] = 1.0f / texture->Width;  // x
    texture->TextureScale[2] = 1.0f / texture->Height; // y
    texture->TextureScale[1] = 0;  // z
    texture->TextureScale[0] = 0;  // w


    int vpWidth = width;
    int vpHeight = height;

    // 3DS does not allow rendering to a viewport whose width > 512.
    //
    if (vpWidth > 512) vpWidth = 512;
    if (vpHeight > 512) vpHeight = 512;


	matrix3dsInitOrthographic(texture->Projection, 0.0f, vpWidth, vpHeight, 0.0f, 0.0f, 1.0f);
	matrix3dsRotateZ(texture->Projection, M_PI / 2.0f);

    GX_MemoryFill(
        (u32*)texture->PixelData, 0x00000000,
        (u32*)&((u8*)texture->PixelData)[texture->BufferSize],
        GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
        NULL, 0x00000000, NULL, 0);
    gspWaitForPSC0();

#ifndef RELEASE
    printf ("clear: %x %d\n", texture->PixelData, texture->BufferSize);
    printf ("Allocated %d x %d in VRAM (%d)\n", width, height, size);
#endif

	return texture;
}

void gpu3dsDestroyTextureFromVRAM(SGPUTexture *texture)
{
    if (texture->PixelData) vramFree(texture->PixelData);
    if (texture) free(texture);
}



void gpu3dsStartNewFrame()
{
    //if (GPU3DS.enableDebug)
    //    printf("  gpu3dsStartNewFrame\n");

    gpuCurrentCommandBuffer = 1 - gpuCurrentCommandBuffer;

    impl3dsPrepareForNewFrame();

    if (gpuCurrentCommandBuffer == 0)
    {
	    GPUCMD_SetBuffer(gpuCommandBuffer1, gpuCommandBufferSize, 0);
    }
    else
    {
	    GPUCMD_SetBuffer(gpuCommandBuffer2, gpuCommandBufferSize, 0);
    }
}


//int currentShaderIndex = -1;

void gpu3dsUseShader(int shaderIndex)
{
    if (GPU3DS.currentShader != shaderIndex)
    {
        GPU3DS.currentShader = shaderIndex;
        shaderProgramUse(&GPU3DS.shaders[shaderIndex].shaderProgram);
        
        u32 *mainScreen = (screenSettings.GameScreen == GFX_TOP) ? (u32 *)GPU3DS.projectionTopScreen : (u32 *)GPU3DS.projectionBottomScreen;
        
        if (!GPU3DS.currentRenderTarget)
        {
            GPU_SetFloatUniform(GPU_VERTEX_SHADER, renderTargetVertexShaderRegister, mainScreen, 4);
            GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, renderTargetGeometryShaderRegister, mainScreen, 4);
        }
        else
        {
            GPU_SetFloatUniform(GPU_VERTEX_SHADER, renderTargetVertexShaderRegister, (u32 *)GPU3DS.currentRenderTarget->Projection, 4);
            GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, renderTargetGeometryShaderRegister, mainScreen, 4);
        }

        if (GPU3DS.currentTexture != NULL)
        {
            GPU_SetFloatUniform(GPU_VERTEX_SHADER, textureVertexShaderRegister, (u32 *)GPU3DS.currentTexture->TextureScale, 1);
            GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, textureGeometryShaderRegister, (u32 *)GPU3DS.currentTexture->TextureScale, 1);
        }

        GPU_SetFloatUniform(GPU_VERTEX_SHADER, textureOffsetVertexShaderRegister, (u32 *)GPU3DS.textureOffset, 1);

    }
}


void gpu3dsInitializeShaderRegistersForRenderTarget(int vertexShaderRegister, int geometryShaderRegister)
{
    renderTargetVertexShaderRegister = vertexShaderRegister;
    renderTargetGeometryShaderRegister = geometryShaderRegister;
}


void gpu3dsInitializeShaderRegistersForTexture(int vertexShaderRegister, int geometryShaderRegister)
{
    textureVertexShaderRegister = vertexShaderRegister;
    textureGeometryShaderRegister = geometryShaderRegister;
}


void gpu3dsInitializeShaderRegistersForTextureOffset(int vertexShaderRegister)
{
    textureOffsetVertexShaderRegister = vertexShaderRegister;
}


void gpu3dsLoadShader(int shaderIndex, u32 *shaderBinary,
    int size, int geometryShaderStride)
{
	GPU3DS.shaders[shaderIndex].dvlb = DVLB_ParseFile((u32 *)shaderBinary, size);
#ifndef RELEASE
    printf ("Load DVLB %x size=%d shader=%d\n", GPU3DS.shaders[shaderIndex].dvlb, size, shaderIndex);
#endif

	shaderProgramInit(&GPU3DS.shaders[shaderIndex].shaderProgram);
	shaderProgramSetVsh(&GPU3DS.shaders[shaderIndex].shaderProgram,
        &GPU3DS.shaders[shaderIndex].dvlb->DVLE[0]);
#ifndef RELEASE
    printf ("  Vertex shader loaded: %x\n", GPU3DS.shaders[shaderIndex].shaderProgram.vertexShader);
#endif

	if (geometryShaderStride)
    {
		shaderProgramSetGsh(&GPU3DS.shaders[shaderIndex].shaderProgram,
			&GPU3DS.shaders[shaderIndex].dvlb->DVLE[1], geometryShaderStride);
#ifndef RELEASE
        printf ("  Geometry shader loaded: %x\n", GPU3DS.shaders[shaderIndex].shaderProgram.geometryShader);
#endif
    }
}

void gpu3dsEnableAlphaBlending()
{
	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsDisableAlphaBlending()
{
	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_ONE, GPU_ZERO,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsDisableAlphaBlendingKeepDestAlpha()
{
    GPU_SetAlphaBlending(
        GPU_BLEND_ADD,
        GPU_BLEND_ADD,
        GPU_ONE, GPU_ZERO,
        GPU_ZERO, GPU_ONE
    );
}

void gpu3dsEnableAdditiveBlending()
{
	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_DST_ALPHA, GPU_ONE,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsEnableSubtractiveBlending()
{
	GPU_SetAlphaBlending(
		GPU_BLEND_REVERSE_SUBTRACT,
		GPU_BLEND_ADD,
		GPU_DST_ALPHA, GPU_ONE,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsEnableAdditiveDiv2Blending()
{
    GPU_SetBlendingColor(0, 0, 0, 0xff);
	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsEnableSubtractiveDiv2Blending()
{
    GPU_SetBlendingColor(0, 0, 0, 0xff);
	GPU_SetAlphaBlending(
		GPU_BLEND_REVERSE_SUBTRACT,
		GPU_BLEND_ADD,
		GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA,
		GPU_ONE, GPU_ZERO
	);
}

void gpu3dsResetState()
{
	GPU_DepthMap(-1.0f, 0.0f);
	GPUCMD_AddWrite(GPUREG_FACECULLING_CONFIG, GPU_CULL_NONE&0x3);
	GPU_SetStencilTest(false, GPU_ALWAYS, 0x00, 0xFF, 0x00);
	GPU_SetStencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
	GPU_SetBlendingColor(0,0,0,0);
	GPU_SetDepthTestAndWriteMask(false, GPU_GEQUAL, GPU_WRITE_ALL);
	GPUCMD_AddMaskedWrite(GPUREG_EARLYDEPTH_TEST1, 0x1, 0);
	GPUCMD_AddWrite(GPUREG_EARLYDEPTH_TEST2, 0);

	GPU_SetAlphaBlending(
		GPU_BLEND_ADD,
		GPU_BLEND_ADD,
		GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		GPU_ONE, GPU_ZERO
	);

	GPU_SetAlphaTest(true, GPU_NOTEQUAL, 0x00);

	gpu3dsClearTextureEnv(1);
	gpu3dsClearTextureEnv(2);
	gpu3dsClearTextureEnv(3);
	gpu3dsClearTextureEnv(4);
	gpu3dsClearTextureEnv(5);

    gpu3dsFlush();
    gpu3dsWaitForPreviousFlush();
}


/*
The following array is based on
    https://www.3dbrew.org/wiki/GPU/Internal_Registers#GPUREG_COLORBUFFER_FORMAT and
supports only the following frame buffer format types:

  GPU_RGBA8 = 0x0,
  GPU_RGB8 = 0x1,
  GPU_RGBA5551 = 0x2,
  GPU_RGB565 = 0x3,
  GPU_RGBA4 = 0x4
*/
const uint32 GPUREG_COLORBUFFER_FORMAT_VALUES[5] = { 0x0002, 0x00010001, 0x00020000, 0x00030000, 0x00040000 };


void gpu3dsSetRenderTargetToFrameBuffer(gfxScreen_t targetScreen)
{
    if (GPU3DS.currentRenderTarget != NULL)
    {
        u32 *screen = (targetScreen == GFX_TOP) ? (u32 *)GPU3DS.projectionTopScreen : (u32 *)GPU3DS.projectionBottomScreen;

        GPU_SetFloatUniform(GPU_VERTEX_SHADER, renderTargetVertexShaderRegister, screen, 4);
        GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, renderTargetGeometryShaderRegister, screen, 4);

        GPU3DS.currentRenderTarget = NULL;

        GPU_SetViewport(
            //(u32 *)osConvertVirtToPhys(GPU3DS.frameDepthBuffer),
            GPU3DS.frameDepthBuffer == NULL ? NULL : (u32 *)osConvertVirtToPhys(GPU3DS.frameDepthBuffer),
            (u32 *)osConvertVirtToPhys(GPU3DS.frameBuffer),
            0, 0, SCREEN_HEIGHT, (targetScreen == GFX_TOP) ? SCREEN_TOP_WIDTH : SCREEN_BOTTOM_WIDTH);

        GPUCMD_AddSingleParam(0x000F0117, GPUREG_COLORBUFFER_FORMAT_VALUES[GPU3DS.frameBufferFormat]); //color buffer format
    }
}


void gpu3dsSetRenderTargetToTexture(SGPUTexture *texture, SGPUTexture *depthTexture)
{
    if (GPU3DS.currentRenderTarget != texture || GPU3DS.currentRenderTargetDepth != depthTexture)
    {
        // Upload saved uniform
        GPU_SetFloatUniform(GPU_VERTEX_SHADER, renderTargetVertexShaderRegister, (u32 *)texture->Projection, 4);
        GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, renderTargetGeometryShaderRegister, (u32 *)texture->Projection, 4);

        GPU3DS.currentRenderTarget = texture;
        GPU3DS.currentRenderTargetDepth = depthTexture;

        int vpWidth = texture->Width;
        int vpHeight = texture->Height;

        // 3DS does not allow rendering to a viewport whose width > 512.
        //
        if (vpWidth > 512) vpWidth = 512;
        if (vpHeight > 512) vpHeight = 512;

        GPU_SetViewport(
            depthTexture == NULL ? NULL : (u32 *)osConvertVirtToPhys(depthTexture->PixelData),
            (u32 *)osConvertVirtToPhys(texture->PixelData),
            0, 0, vpWidth, vpHeight);

        GPUCMD_AddSingleParam(0x000F0117, GPUREG_COLORBUFFER_FORMAT_VALUES[texture->PixelFormat]); //color buffer format
    }
}


void gpu3dsSetRenderTargetToTextureSpecific(SGPUTexture *texture, SGPUTexture *depthTexture, int addressOffset, int width, int height)
{
    // Upload saved uniform
    GPU_SetFloatUniform(GPU_VERTEX_SHADER, renderTargetVertexShaderRegister, (u32 *)texture->Projection, 4);
    GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, renderTargetGeometryShaderRegister, (u32 *)texture->Projection, 4);

    GPU3DS.currentRenderTarget = texture;
    GPU3DS.currentRenderTargetDepth = depthTexture;

    GPU_SetViewport(
        depthTexture == NULL ? NULL : (u32 *)osConvertVirtToPhys(depthTexture->PixelData),
        (u32 *)osConvertVirtToPhys((void *)((int)texture->PixelData + addressOffset)),
        0, 0, width, height);

    GPUCMD_AddSingleParam(0x000F0117, GPUREG_COLORBUFFER_FORMAT_VALUES[texture->PixelFormat]); //color buffer format
}

void gpu3dsFlush()
{
	u32* commandBuffer;
	u32  commandBuffer_size;
    
	if(somethingWasDrawn) {
	    GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0x00000000);
	    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 0x00000001);
	    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 0x00000001);
    }
    
	GPUCMD_Split(&commandBuffer, &commandBuffer_size);
	GX_FlushCacheRegions (commandBuffer, commandBuffer_size * 4, (u32 *) __ctru_linear_heap, __ctru_linear_heap_size, NULL, 0);
	GX_ProcessCommandList(commandBuffer, commandBuffer_size * 4, 0x00);
    
    somethingWasFlushed = true;
    somethingWasDrawn = false;


}

void gpu3dsWaitForPreviousFlush()
{
    if (somethingWasFlushed)
    {
        if (GPU3DS.isReal3DS)   // Don't bother waiting in the Citra emulator (it can freeze sometimes!)
            gspWaitForP3D();
        somethingWasFlushed = false;
    }

}

/*
Translate from the following GPU_TEXCOLOR to their respective GX_TRANSFER_FMT values.
  GPU_RGBA8 = 0x0,
  GPU_RGB8 = 0x1,
  GPU_RGBA5551 = 0x2,
  GPU_RGB565 = 0x3,
  GPU_RGBA4 = 0x4
*/
const uint32 GX_TRANSFER_FRAMEBUFFER_FORMAT_VALUES[5] = {
    GX_TRANSFER_FMT_RGBA8, GX_TRANSFER_FMT_RGB8, GX_TRANSFER_FMT_RGB5A1, GX_TRANSFER_FMT_RGB565, GX_TRANSFER_FMT_RGBA4 };

/*
Translate from the following GSPGPU_FramebufferFormat to their respective GX_TRANSFER_FMT values:
  GSP_RGBA8_OES =0,
  GSP_BGR8_OES =1,
  GSP_RGB565_OES =2,
  GSP_RGB5_A1_OES =3,
  GSP_RGBA4_OES =4
*/
const uint32 GX_TRANSFER_SCREEN_FORMAT_VALUES[5]= {
    GX_TRANSFER_FMT_RGBA8, GX_TRANSFER_FMT_RGB8, GX_TRANSFER_FMT_RGB565, GX_TRANSFER_FMT_RGB5A1, GX_TRANSFER_FMT_RGBA4 };


void gpu3dsTransferToScreenBuffer(gfxScreen_t screen)
{
    int screenWidth = (screen == screenSettings.GameScreen) ? screenSettings.GameScreenWidth : screenSettings.SecondScreenWidth;
    gpu3dsWaitForPreviousFlush();
    GX_DisplayTransfer(GPU3DS.frameBuffer, GX_BUFFER_DIM(SCREEN_HEIGHT, screenWidth),
        (u32 *)gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL),
        GX_BUFFER_DIM(SCREEN_HEIGHT, screenWidth),
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FRAMEBUFFER_FORMAT_VALUES[GPU3DS.frameBufferFormat]) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_SCREEN_FORMAT_VALUES[GPU3DS.screenFormat]));
}

void gpu3dsSwapScreenBuffers()
{
    gfxScreenSwapBuffers(GFX_TOP, false);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}


void gpu3dsBindTextureWithParams(SGPUTexture *texture, GPU_TEXUNIT unit, u32 param)
{
    if (GPU3DS.currentTexture != texture || GPU3DS.currentParams != param)
    {
        GPU_SetTextureEnable(unit);

        GPU_SetTexEnv(
            0,
            GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
            GPU_TEVSOURCES(GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0),
            GPU_TEVOPERANDS(0, 0, 0),
            GPU_TEVOPERANDS(0, 0, 0),
            GPU_REPLACE, GPU_REPLACE,
            0x80808080
        );

        GPU_SetTexture(
            unit,
            (u32 *)osConvertVirtToPhys(texture->PixelData),
            texture->Width,
            texture->Height,
            param,
            texture->PixelFormat
        );

        GPU_SetFloatUniform(GPU_VERTEX_SHADER, textureVertexShaderRegister, (u32 *)texture->TextureScale, 1);
        GPU_SetFloatUniform(GPU_GEOMETRY_SHADER, textureGeometryShaderRegister, (u32 *)texture->TextureScale, 1);

        GPU3DS.currentTexture = texture;
        GPU3DS.currentParams = param;
    }
}


void gpu3dsBindTexture(SGPUTexture *texture, GPU_TEXUNIT unit)
{
    gpu3dsBindTextureWithParams(texture, unit, texture->Params);
}



void gpu3dsScissorTest(GPU_SCISSORMODE mode, uint32 x, uint32 y, uint32 w, uint32 h)
{
    GPU_SetScissorTest(mode, x, y, w, h);
}



void gpu3dsSetTextureOffset(float u, float v)
{
    GPU3DS.textureOffset[3] = u;
    GPU3DS.textureOffset[2] = v;
    GPU_SetFloatUniform(GPU_VERTEX_SHADER, textureOffsetVertexShaderRegister, (u32 *)GPU3DS.textureOffset, 1);    
}
