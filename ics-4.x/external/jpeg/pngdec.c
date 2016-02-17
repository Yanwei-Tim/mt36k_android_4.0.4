
/* pngdec.c - location for general purpose libpng functions
 *
 * 
 */


//-----------------------------------------------------------------------------
// Include files
//-----------------------------------------------------------------------------
#include "mtimage.h"
#include "drv_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <memory.h>
#include "jpeglib.h"
#include <pthread.h>
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifdef PNG_HW_SUPPORT
static volatile BOOL fgMmplayEOF = FALSE;
static volatile BOOL fgImgRetErr=FALSE;
static UINT8 _u1ImgPlayStatus = 0;
pthread_cond_t pngStopCond;
pthread_mutex_t pngStopMutex ;
pthread_mutex_t pngHwInstanceMutex = PTHREAD_MUTEX_INITIALIZER;

static VOID _PngCallBack(
    MTIMAGE_CALLBACK_TYPE_T eCallbackType,
    UINT32 u4ErrorType,UINT32 u4Param)
{
    UINT8 u4msg = 0;
    switch (eCallbackType)
    {
    case MT_IMAGE_CALLBACK_ERROR:      // notification of play error
        u4msg = 2;
		SkJpegDebugf("PNG ERROR...");
        fgImgRetErr = TRUE;
        break;
    case MT_IMAGE_CALLBACK_FINISH:
		SkJpegDebugf("PNG EOF...");
        u4msg = 1;
        break;
    case MT_IMAGE_CALLBACK_NEEDDATA:
		SkJpegDebugf("PNG NEED DATA...");
        return;
    case MT_IMAGE_CALLBACK_STOPDONE:
        u4msg = 3;
        SkJpegDebugf("PNG STOP DONE.\n");
        break;
    case MT_IMAGE_CALLBACK_ALREADYSTOPED:
        u4msg = 4;
        SkJpegDebugf("PNG ALREADY STOPEED.\n");
		break;
    default:
        _u1ImgPlayStatus = (UINT8)5;
        return;
    }
    _u1ImgPlayStatus = (UINT8)u4msg;
}
void* png_io_ptr = NULL;
MTIMAGE_PNG_READ png_Read;
static boolean fgInited = FALSE;
static BOOL fgStop = FALSE;
static BOOL fgHwDecoding = FALSE;
static BOOL _bWaitingPngStop = FALSE;

void mtimage_set_read_fn(void* io_ptr, MTIMAGE_PNG_READ fnPngRead)
{
	png_io_ptr = io_ptr;
	png_Read = fnPngRead;
	return;
}
UINT32 png_read_ex(UINT32 u4Addr,UINT32 u4Len)
{
    MTIMAGE_PNG_READ fn_png_read = png_Read;
	UINT32 retLen = 0;
	if((fn_png_read) && (!fgStop))
	{
	   retLen = fn_png_read(png_io_ptr,u4Addr,u4Len);
	}
    return retLen;
}

boolean fgPngGetHwInstance(void)
{
    INT32 i4Ret = 0;

    i4Ret = pthread_mutex_trylock(&pngHwInstanceMutex);
    SkJpegDebugf("fgPngGetHwInstance get hw instance ret=%d!\n", i4Ret);
	if(0 == i4Ret)
    {
        return TRUE;
    }
    else
    {
        SkJpegDebugf("fgPngGetHwInstance get hw instance failed ret=%d!\n", i4Ret);
    }
    return FALSE;
}

boolean fgPngReleaseHwInstance(void)
{
    INT32 i4Ret = 0;
    i4Ret = pthread_mutex_unlock(&pngHwInstanceMutex);
    SkJpegDebugf("fgPngReleaseHwInstance release hw instance ret=%d!\n", i4Ret);
	if(0 == i4Ret)
    {
        return TRUE;
    }
    else
    {
        SkJpegDebugf("fgPngReleaseHwInstance release hw instance failed ret=%d!\n", i4Ret);
    }
    return FALSE;
}

boolean fgPngIsFree(void)
{
    MTIMAGE_STATUS_T rStatus;
    if (!fgInited)
    {
        MTAL_Init();
        MTVDO_Init();
        fgInited =TRUE;
        SkJpegDebugf("fgPngIsFree:first init\n");
    }
    
    if (MTIMAGE_GetInfo(HW_PNG_DEC, NULL_MTIMAGE_HANDLE, &rStatus) == MTR_OK)
    {
        return TRUE;
    }
   	SkJpegDebugf("fgPngIsFree:hardware busy!!! 0x%x\n", rStatus.u4CurImageAddrOffset);    
    return FALSE;
}

boolean mtimage_png_get_header(UINT32 * color_type, UINT32 * bDepth, UINT32 * tRNSExist, UINT32 * trans_num, UINT32 *is9patch, UINT32 *u49patchAddr)
{
	MT_RESULT_T eImgRet = MTIMAGE_GetPngHdr(NULL_MTIMAGE_HANDLE, color_type, bDepth, tRNSExist, trans_num, is9patch, u49patchAddr);    
    if(MTR_OK != eImgRet)
    {
	   	SkJpegDebugf("mtimage_png_get_header failed error:0x%x\n", eImgRet);
        return FALSE;
    }
    return TRUE;
}

boolean mtimage_png_dec_start(UINT16 sampleSize,UINT32 fileSize)
{
	MTIMAGE_HANDLE hMtimgHandle = NULL;
	MT_RESULT_T eImgRet = MTR_OK;
	MTIMAGE_HANDLE pImageItem = NULL;
    MTIMAGE_STATUS_T rStatus;
	pthread_cond_init(&pngStopCond, NULL);
	pthread_cond_init(&pngStopMutex, NULL);
	fgHwDecoding = TRUE;
	fgImgRetErr = FALSE;
	_u1ImgPlayStatus = 0;
	fgStop = FALSE;
    MTIMAGE_SetStopFlag(HW_PNG_DEC, fgStop);
	
	MTIMAGE_Connect(HW_PNG_DEC,TRUE,0);
    SkJpegDebugf("mtimage_png_dec_start MTIMAGE_DataFromBuffer\n");    
	eImgRet = MTIMAGE_DataFromBuffer(HW_PNG_DEC, &pImageItem, (UINT8*)0x200, 0, (UINT32)1);
	MTIMAGE_RegCb(HW_PNG_DEC, pImageItem, _PngCallBack, 0);
	MTIMAGE_RegReadFuc(HW_PNG_DEC, pImageItem,png_read_ex,NULL);
	MTIMAGE_SetDecFileSize(HW_PNG_DEC, NULL, fileSize);
	MTIMAGE_SetDecRatio(HW_PNG_DEC, NULL, sampleSize);
	if (MTIMAGE_Start(HW_PNG_DEC, pImageItem) != 0)
	{	
		return FALSE;
	}
	
	MTIMAGE_Filldata_Libjpeg(HW_PNG_DEC);
	if (_u1ImgPlayStatus != 1)
	{
		return FALSE;
	}
	
    return TRUE;
}

boolean mtimage_png_getImg(UINT32* outputWidth, UINT32* outputHeight, UINT32* outputAddr, UINT32 osdMode,boolean doDither)
{
    UINT32 u4Buf = 0, u4PhyBuf = 0,u4CbCrOffset = 0;
    MTVDO_REGION_T rRegion;
    if((outputWidth == NULL) || (outputHeight == NULL) || (outputAddr == NULL))
    {
        return FALSE;
    }
    SkJpegDebugf("mtimage_png_getImg osdmode %d, doDither %d!\n", osdMode,doDither);
    MTIMAGE_GetImg(HW_PNG_DEC, NULL, &u4Buf, &u4PhyBuf,&u4CbCrOffset,&rRegion, osdMode,doDither);
    *outputWidth = rRegion.u4Width;
    *outputHeight = rRegion.u4Height;
    *outputAddr = u4Buf ;

    return TRUE;
}

boolean mtimage_png_read_rows(UINT8* uAddr, int lines,UINT32 outputWidth, UINT32 u4Addr, UINT32 osdMode)
{
	UINT32 u4Pitch = (outputWidth + 0xf) & ~0xf;
	UINT8 *addrp = (UINT8*)u4Addr;
	UINT8 Alpha;
	int srcBytePerPixel = 4;
	int dstBytePerPixel = 4;
	int i = 0;
	if((addrp == NULL) || (uAddr == NULL))
	{
	   SkJpegDebugf("mtimage_png_read_rows addr null!!!\n");
	   return FALSE;
	}
    if(JPG_GETIMG_OSDMODE_ARGB565 == osdMode)
    {
#if 0   /*software 8888 to 565*/
        UINT8 low, high, R, G, B;
        int srcBytePerPixel = 4;
        int dstBytePerPixel = 2;
	for (i = 0;i < outputWidth;i++)
   	{
            Alpha = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+3 );//A
            R = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+2);//R
            G = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+1);//G
            B = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+0);//B

            low  = B>>3;
            low |= (G>>2)<<5;
            
            high = (G>>5);
            high|= (R & 0xf8);
            
            *(uAddr + i*dstBytePerPixel + 0) = low;
            *(uAddr + i*dstBytePerPixel + 1) = high;
        }
#else   /*8888 to 565 already done by hardware*/
        UINT8 low, high;
        srcBytePerPixel = 2;
        dstBytePerPixel = 2;
        for (i = 0;i < outputWidth;i++)
        {
            low  = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+0);
            high = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+1);

            *(uAddr + i*dstBytePerPixel + 0) = low;
            *(uAddr + i*dstBytePerPixel + 1) = high;
        }
#endif
    }
    else
    {
		for (i = 0;i < outputWidth;i++)
	   	{
			*(uAddr + i*dstBytePerPixel + 3) = Alpha = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+3 );//A
			*(uAddr + i*dstBytePerPixel ) = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel);//R
			*(uAddr + i*dstBytePerPixel + 1) = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+1);//G
		    *(uAddr + i*dstBytePerPixel + 2) = *(addrp + lines * u4Pitch * srcBytePerPixel + i * srcBytePerPixel+2);//B
		}
    } 
   return TRUE;
}

void mtimage_png_dec_stop(void)
{
	usleep(500);
	if(fgHwDecoding)
	{
		if((!fgPngIsFree()) && (!fgStop))
		{
		    fgStop = TRUE;
			MTIMAGE_SetStopFlag(HW_PNG_DEC, fgStop);
			MTIMAGE_Stop(HW_PNG_DEC, NULL_MTIMAGE_HANDLE);
		   	SkJpegDebugf("wait stop png finish...tid %d pid %d\n",gettid(),getpid());
			pthread_mutex_lock(&pngStopMutex);
			_bWaitingPngStop = TRUE;
			pthread_cond_wait(&pngStopCond, &pngStopMutex);
			_bWaitingPngStop = FALSE;
		    fgStop = FALSE;
			MTIMAGE_SetStopFlag(HW_PNG_DEC, fgStop);
			pthread_mutex_unlock(&pngStopMutex);
			SkJpegDebugf("stop finish\n");
		}
	}
	return;
}

boolean mtimage_png_dec_close(void)
{
	MTIMAGE_Close(HW_PNG_DEC, NULL_MTIMAGE_HANDLE);
	SkJpegDebugf("mtimage_png_dec_close...fgStop %d\n", fgStop);
	//SkJpegDebugf("Getpid %d GetTid %d\n",getpid(),gettid());
	fgHwDecoding = FALSE;
	if((fgStop) || (_u1ImgPlayStatus == 3) || (_u1ImgPlayStatus == 4))
	{
		pthread_mutex_lock(&pngStopMutex);
		while(!_bWaitingPngStop)
		{
			SkJpegDebugf("mtimage_png_dec_close while loop.....\n");
			pthread_mutex_unlock(&pngStopMutex);
			usleep(200);			
			pthread_mutex_lock(&pngStopMutex);
		}
		pthread_cond_signal(&pngStopCond);
		pthread_mutex_unlock(&pngStopMutex);
	
	}
	return TRUE;
}

boolean mtimage_write_to_usb(UINT8* DstAddr,long filelenth)
{
   char *filepath;
   FILE * filehandle;
   SkJpegDebugf("begin to write result to usb....\n");
   
   filepath = "/mnt/usbdisk/test.bin";
   filehandle = fopen(filepath,"wb");
   if(filehandle == NULL)
   {
		SkJpegDebugf("open file fail\n");
      return FALSE;
   }
   
   SkJpegDebugf("begin write to file\n");
   fwrite(DstAddr,1,filelenth,filehandle);
   SkJpegDebugf("write to file finished\n");
   fclose(filehandle);
   return TRUE;
}

boolean mtimage_memcpy(UINT32 u4DstAddr ,UINT32 u4SrcAddr,UINT32 u4Size)
{
	memcpy((void*)u4DstAddr,(void*)u4SrcAddr,u4Size);
	return TRUE;
}
#endif
