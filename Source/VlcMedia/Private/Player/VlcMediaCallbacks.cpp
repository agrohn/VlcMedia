// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "VlcMediaCallbacks.h"
#include "VlcMediaPrivate.h"

#include "IMediaAudioSample.h"
#include "IMediaOptions.h"
#include "IMediaTextureSample.h"
#include "MediaSamples.h"

#include "Vlc.h"
#include "VlcMediaAudioSample.h"
#include "VlcMediaTextureSample.h"
#include <Engine/Texture2D.h>

/* FVlcMediaOutput structors
 *****************************************************************************/

FVlcMediaCallbacks::FVlcMediaCallbacks()
	: AudioChannels(0)
	, AudioSampleFormat(EMediaAudioSampleFormat::Int16)
	, AudioSamplePool(new FVlcMediaAudioSamplePool)
	, AudioSampleRate(0)
	, AudioSampleSize(0)
	, CurrentTime(FTimespan::Zero())
	, Player(nullptr)
	, Samples(new FMediaSamples)
	, VideoBufferDim(FIntPoint::ZeroValue)
	, VideoBufferStride(0)
	, VideoFrameDuration(FTimespan::Zero())
	, VideoOutputDim(FIntPoint::ZeroValue)
	, VideoPreviousTime(FTimespan::MinValue())
	, VideoSampleFormat(EMediaTextureSampleFormat::CharAYUV)
	, VideoSamplePool(new FVlcMediaTextureSamplePool)
	, VideoTexture2D( nullptr )

{ }


FVlcMediaCallbacks::~FVlcMediaCallbacks()
{
	Shutdown();

	delete AudioSamplePool;
	AudioSamplePool = nullptr;

	delete Samples;
	Samples = nullptr;

	delete VideoSamplePool;
	VideoSamplePool = nullptr;
}


/* FVlcMediaOutput interface
 *****************************************************************************/

IMediaSamples& FVlcMediaCallbacks::GetSamples()
{
	return *Samples;
}


void UpdateTextureRegions(UTexture2D* Texture, int32 MipIndex, uint32 NumRegions, FUpdateTextureRegion2D* Regions, uint32 SrcPitch, uint32 SrcBpp, uint8* SrcData, bool bFreeData)
{
	if (Texture->Resource)
	{
		struct FUpdateTextureRegionsData
		{
			FTexture2DResource* Texture2DResource;
			int32 MipIndex;
			uint32 NumRegions;
			FUpdateTextureRegion2D* Regions;
			uint32 SrcPitch;
			uint32 SrcBpp;
			uint8* SrcData;
		};

		FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;

		RegionData->Texture2DResource = (FTexture2DResource*)Texture->Resource;
		RegionData->MipIndex = MipIndex;
		RegionData->NumRegions = NumRegions;
		RegionData->Regions = Regions;
		RegionData->SrcPitch = SrcPitch;
		RegionData->SrcBpp = SrcBpp;
		RegionData->SrcData = SrcData;

		ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
			UpdateTextureRegionsData,
			FUpdateTextureRegionsData*, RegionData, RegionData,
			bool, bFreeData, bFreeData,
			{
			for (uint32 RegionIndex = 0; RegionIndex < RegionData->NumRegions; ++RegionIndex)
			{
				int32 CurrentFirstMip = RegionData->Texture2DResource->GetCurrentFirstMip();
				if (RegionData->MipIndex >= CurrentFirstMip)
				{
					RHIUpdateTexture2D(
						RegionData->Texture2DResource->GetTexture2DRHI(),
						RegionData->MipIndex - CurrentFirstMip,
						RegionData->Regions[RegionIndex],
						RegionData->SrcPitch,
						RegionData->SrcData
						+ RegionData->Regions[RegionIndex].SrcY * RegionData->SrcPitch
						+ RegionData->Regions[RegionIndex].SrcX * RegionData->SrcBpp
						);
				}
			}
			if (bFreeData)
			{
				FMemory::Free(RegionData->Regions);
				FMemory::Free(RegionData->SrcData);
			}
			delete RegionData;
		});
	}
}

void FVlcMediaCallbacks::Initialize(FLibvlcMediaPlayer& InPlayer)
{
	Shutdown();

	Player = &InPlayer;

	// register callbacks
	FVlc::AudioSetFormatCallbacks(
		Player,
		&FVlcMediaCallbacks::StaticAudioSetupCallback,
		&FVlcMediaCallbacks::StaticAudioCleanupCallback
	);

	FVlc::AudioSetCallbacks(
		Player,
		&FVlcMediaCallbacks::StaticAudioPlayCallback,
		&FVlcMediaCallbacks::StaticAudioPauseCallback,
		&FVlcMediaCallbacks::StaticAudioResumeCallback,
		&FVlcMediaCallbacks::StaticAudioFlushCallback,
		&FVlcMediaCallbacks::StaticAudioDrainCallback,
		this
	);

	FVlc::VideoSetFormatCallbacks(
		Player,
		&FVlcMediaCallbacks::StaticVideoSetupCallback,
		&FVlcMediaCallbacks::StaticVideoCleanupCallback
	);

	FVlc::VideoSetCallbacks(
		Player,
		&FVlcMediaCallbacks::StaticVideoLockCallback,
		&FVlcMediaCallbacks::StaticVideoUnlockCallback,
		&FVlcMediaCallbacks::StaticVideoDisplayCallback,
		this
	);
}


void FVlcMediaCallbacks::Shutdown()
{
	if (Player == nullptr)
	{
		return;
	}

	// unregister callbacks
	FVlc::AudioSetCallbacks(Player, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
	FVlc::AudioSetFormatCallbacks(Player, nullptr, nullptr);

	FVlc::VideoSetCallbacks(Player, nullptr, nullptr, nullptr, nullptr);
	FVlc::VideoSetFormatCallbacks(Player, nullptr, nullptr);

	AudioSamplePool->Reset();
	VideoSamplePool->Reset();

	CurrentTime = FTimespan::Zero();
	Player = nullptr;
}


/* FVlcMediaOutput static functions
*****************************************************************************/

void FVlcMediaCallbacks::StaticAudioCleanupCallback(void* Opaque)
{
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioCleanupCallback"), Opaque);
}


void FVlcMediaCallbacks::StaticAudioDrainCallback(void* Opaque)
{
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioDrainCallback"), Opaque);
}


void FVlcMediaCallbacks::StaticAudioFlushCallback(void* Opaque, int64 Timestamp)
{
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioFlushCallback"), Opaque);
}


void FVlcMediaCallbacks::StaticAudioPauseCallback(void* Opaque, int64 Timestamp)
{
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioPauseCallback (Timestamp = %i)"), Opaque, Timestamp);

	// do nothing; pausing is handled in Update
}


void FVlcMediaCallbacks::StaticAudioPlayCallback(void* Opaque, void* Samples, uint32 Count, int64 Timestamp)
{
	auto Callbacks = (FVlcMediaCallbacks*)Opaque;

	if (Callbacks == nullptr)
	{
		return;
	}

	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioPlayCallback (Count = %i, Timestamp = %i, Queue = %i)"),
		Opaque,
		Count,
		Timestamp,
		Callbacks->Samples->NumAudio()
	);

	// create & add sample to queue
	auto AudioSample = Callbacks->AudioSamplePool->AcquireShared();

	const FTimespan Delay = FTimespan::FromMicroseconds(FVlc::Delay(Timestamp));
	const FTimespan Duration = FTimespan::FromMicroseconds((Count * 1000000) / Callbacks->AudioSampleRate);
	const SIZE_T SamplesSize = Count * Callbacks->AudioSampleSize * Callbacks->AudioChannels;

	if (AudioSample->Initialize(
		Samples,
		SamplesSize,
		Count,
		Callbacks->AudioChannels,
		Callbacks->AudioSampleFormat,
		Callbacks->AudioSampleRate,
		Callbacks->CurrentTime + Delay,
		Duration))
	{
		Callbacks->Samples->AddAudio(AudioSample);
	}
}


void FVlcMediaCallbacks::StaticAudioResumeCallback(void* Opaque, int64 Timestamp)
{
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioResumeCallback (Timestamp = %i)"), Opaque, Timestamp);

	// do nothing; resuming is handled in Update
}


int FVlcMediaCallbacks::StaticAudioSetupCallback(void** Opaque, ANSICHAR* Format, uint32* Rate, uint32* Channels)
{
	auto Callbacks = *(FVlcMediaCallbacks**)Opaque;

	if (Callbacks == nullptr)
	{
		return -1;
	}

	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticAudioSetupCallback (Format = %s, Rate = %i, Channels = %i)"),
		Opaque,
		ANSI_TO_TCHAR(Format),
		*Rate,
		*Channels
	);

	// setup audio format
	if (*Channels > 8)
	{
		*Channels = 8;
	}

	if (FMemory::Memcmp(Format, "S8  ", 4) == 0)
	{
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Int8;
		Callbacks->AudioSampleSize = 1;
	}
	else if (FMemory::Memcmp(Format, "S16N", 4) == 0)
	{
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Int16;
		Callbacks->AudioSampleSize = 2;
	}
	else if (FMemory::Memcmp(Format, "S32N", 4) == 0)
	{
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Int32;
		Callbacks->AudioSampleSize = 4;
	}
	else if (FMemory::Memcmp(Format, "FL32", 4) == 0)
	{
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Float;
		Callbacks->AudioSampleSize = 4;
	}
	else if (FMemory::Memcmp(Format, "FL64", 4) == 0)
	{
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Double;
		Callbacks->AudioSampleSize = 8;
	}
	else if (FMemory::Memcmp(Format, "U8  ", 4) == 0)
	{
		// unsigned integer fall back
		FMemory::Memcpy(Format, "S8  ", 4);
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Int8;
		Callbacks->AudioSampleSize = 1;
	}
	else
	{
		// unsupported format fall back
		FMemory::Memcpy(Format, "S16N", 4);
		Callbacks->AudioSampleFormat = EMediaAudioSampleFormat::Int16;
		Callbacks->AudioSampleSize = 2;
	}

	Callbacks->AudioChannels = *Channels;
	Callbacks->AudioSampleRate = *Rate;

	return 0;
}


void FVlcMediaCallbacks::StaticVideoCleanupCallback(void *Opaque)
{
	// do nothing
}


void FVlcMediaCallbacks::StaticVideoDisplayCallback(void* Opaque, void* Picture)
{
	auto Callbacks = (FVlcMediaCallbacks*)Opaque;
	auto VideoSample = (FVlcMediaTextureSample*)Picture;
	
	if ((Callbacks == nullptr) || (VideoSample == nullptr))
	{
		return;
	}
	
	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticVideoDisplayCallback (CurrentTime = %s, Queue = %i)"),
		Opaque, *Callbacks->CurrentTime.ToString(),
		Callbacks->Samples->NumVideoSamples()
	);

	VideoSample->SetTime(Callbacks->CurrentTime);
	UE_LOG(LogTemp, Warning, TEXT("Got sample right here."));
	uint8 * buffer = (uint8 *)VideoSample->GetMutableBuffer();
	
	
	
	EMediaTextureSampleFormat format = VideoSample->GetFormat();
	FIntPoint dimensions = VideoSample->GetDim();
	UE_LOG(LogTemp, Log, TEXT("Format is%dx%d, %s"), dimensions.X, dimensions.Y, MediaTextureSampleFormat::EnumToString(format));
	
	if ( format == EMediaTextureSampleFormat::CharBGRA )
	{
		uint32 SrcPitch = 4*dimensions.X;
		uint32 SrcBpp = 4;
		
		
		// Mangle data to stick with 
		for(size_t i = 0;i<SrcBpp*dimensions.X*dimensions.Y;i+=SrcBpp)
		{
			
			// In encoding, we have RGB channels in big endian order. 
			// Blue channel holds depth information. RGB is reduced into 5-bit component representation,
			// and stored in R and G bits. 
			//      B        G        R           
			//  00000000 00000000 00011111  R
			//  00000000 00000011 11100000  G
			//  00000000 01111100 00000000  B
			//  11111111 00000000 00000000  A = depth 
			
			// But when in video, we get bits in BGRA order, meaning in big endian:
            //      A        R       G        B        
			//  00000000 00011111 00000000 00000000 R straight
			//  00000000 11100000 00000011 00000000 G (needs more mangling)
			//  00000000 00000000 01111100 00000000 B ( >> 10 )
			//  00000000 00000000 00000000 11111111 
	        // and not forgetting to ignore alpha channel 
			uint32 pixel = *(uint32 *)&buffer[i];
			
			// alpha component (needs to be done first)
			buffer[i+3] = buffer[i];
			// Blue component
			buffer[i]   = (pixel & 0x7C00) >> 7;
			// Green 
			buffer[i+1] = 0;//(pixel & 0x300) >> 2 | (pixel & 0xE00000) >> 18;
			// Red 
			buffer[i+2] = 0;//(pixel & 0x1F0000) >> 13;
			
		}
		if ( Callbacks->VideoTexture2D != nullptr )
		{
			UE_LOG(LogTemp, Log, TEXT("Updating video texture now...%d and %d" ),  buffer[3], buffer[4*dimensions.X/2+3] );
			UpdateTextureRegions(Callbacks->VideoTexture2D, 0, 1, &Callbacks->UpdateRegion, SrcPitch, SrcBpp, buffer, false);
		}
	}
	
	// add sample to queue
	Callbacks->Samples->AddVideo(Callbacks->VideoSamplePool->ToShared(VideoSample));
}


void* FVlcMediaCallbacks::StaticVideoLockCallback(void* Opaque, void** Planes)
{
	auto Callbacks = (FVlcMediaCallbacks*)Opaque;
	check(Callbacks != nullptr);

	FMemory::Memzero(Planes, FVlc::MaxPlanes * sizeof(void*));

	// skip if already processed
	if (Callbacks->VideoPreviousTime == Callbacks->CurrentTime)
	{
		// VLC currently requires a valid buffer or it will crash
		Planes[0] = FMemory::Malloc(Callbacks->VideoBufferStride * Callbacks->VideoBufferDim.Y, 32);
		return nullptr;
	}

	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticVideoLockCallback (CurrentTime = %s)"),
		Opaque,
		*Callbacks->CurrentTime.ToString()
	);

	// create & initialize video sample
	auto VideoSample = Callbacks->VideoSamplePool->Acquire();

	if (VideoSample == nullptr)
	{
		// VLC currently requires a valid buffer or it will crash
		Planes[0] = FMemory::Malloc(Callbacks->VideoBufferStride * Callbacks->VideoBufferDim.Y, 32);
		return nullptr;
	}

	if (!VideoSample->Initialize(
		Callbacks->VideoBufferDim,
		Callbacks->VideoOutputDim,
		Callbacks->VideoSampleFormat,
		Callbacks->VideoBufferStride,
		Callbacks->VideoFrameDuration))
	{
		// VLC currently requires a valid buffer or it will crash
		Planes[0] = FMemory::Malloc(Callbacks->VideoBufferStride * Callbacks->VideoBufferDim.Y, 32);
		return nullptr;
	}

	Callbacks->VideoPreviousTime = Callbacks->CurrentTime;
	Planes[0] = VideoSample->GetMutableBuffer();

	return VideoSample; // passed as Picture into unlock & display callbacks

}


unsigned FVlcMediaCallbacks::StaticVideoSetupCallback(void** Opaque, char* Chroma, unsigned* Width, unsigned* Height, unsigned* Pitches, unsigned* Lines)
{
	auto Callbacks = *(FVlcMediaCallbacks**)Opaque;
	
	if (Callbacks == nullptr)
	{
		return 0;
	}

	UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticVideoSetupCallback (Chroma = %s, Dim = %ix%i)"),
		Opaque,
		ANSI_TO_TCHAR(Chroma),
		*Width,
		*Height
	);

	// get video output size
	if (FVlc::VideoGetSize(Callbacks->Player, 0, (uint32*)&Callbacks->VideoOutputDim.X, (uint32*)&Callbacks->VideoOutputDim.Y) != 0)
	{
		Callbacks->VideoBufferDim = FIntPoint::ZeroValue;
		Callbacks->VideoOutputDim = FIntPoint::ZeroValue;
		Callbacks->VideoBufferStride = 0;

		return 0;
	}

	if (Callbacks->VideoOutputDim.GetMin() <= 0)
	{
		return 0;
	}

	// determine decoder & sample formats
	Callbacks->VideoBufferDim = FIntPoint(*Width, *Height);

	if (FCStringAnsi::Stricmp(Chroma, "AYUV") == 0)
	{
		Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharAYUV;
		Callbacks->VideoBufferStride = *Width * 4;
	}
	else if (FCStringAnsi::Stricmp(Chroma, "RV32") == 0)
	{
		Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharBGRA;
		Callbacks->VideoBufferStride = *Width * 4;
	}
	else if ((FCStringAnsi::Stricmp(Chroma, "UYVY") == 0) ||
		(FCStringAnsi::Stricmp(Chroma, "Y422") == 0) ||
		(FCStringAnsi::Stricmp(Chroma, "UYNV") == 0) ||
		(FCStringAnsi::Stricmp(Chroma, "HDYC") == 0))
	{
		Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharUYVY;
		Callbacks->VideoBufferStride = *Width * 2;
	}
	else if ((FCStringAnsi::Stricmp(Chroma, "YUY2") == 0) ||
		(FCStringAnsi::Stricmp(Chroma, "V422") == 0) ||
		(FCStringAnsi::Stricmp(Chroma, "YUYV") == 0))
	{
		Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharYUY2;
		Callbacks->VideoBufferStride = *Width * 2;
	}
	else if (FCStringAnsi::Stricmp(Chroma, "YVYU") == 0)
	{
		Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharYVYU;
		Callbacks->VideoBufferStride = *Width * 2;
	}
	else
	{
		// reconfigure output for natively supported format
		FLibvlcChromaDescription* ChromaDescr = FVlc::FourccGetChromaDescription(*(FLibvlcFourcc*)Chroma);

		if (ChromaDescr->PlaneCount == 0)
		{
			return 0;
		}

		if (ChromaDescr->PlaneCount > 1)
		{
			FMemory::Memcpy(Chroma, "YUY2", 4);

			Callbacks->VideoBufferDim = FIntPoint(Align(Callbacks->VideoOutputDim.X, 16) / 2, Align(Callbacks->VideoOutputDim.Y, 16));
			Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharYUY2;
			Callbacks->VideoBufferStride = Callbacks->VideoBufferDim.X * 4;
			*Height = Callbacks->VideoBufferDim.Y;
		}
		else
		{
			FMemory::Memcpy(Chroma, "RV32", 4);

			Callbacks->VideoBufferDim = Callbacks->VideoOutputDim;
			Callbacks->VideoSampleFormat = EMediaTextureSampleFormat::CharBGRA;
			Callbacks->VideoBufferStride = Callbacks->VideoBufferDim.X * 4;
		}
	}

	// get other video properties
	Callbacks->VideoFrameDuration = FTimespan::FromSeconds(1.0 / FVlc::MediaPlayerGetFps(Callbacks->Player));

	// initialize decoder
	Lines[0] = Callbacks->VideoBufferDim.Y;
	Pitches[0] = Callbacks->VideoBufferStride;

	return 1;
}


void FVlcMediaCallbacks::StaticVideoUnlockCallback(void* Opaque, void* Picture, void* const* Planes)
{
	if ((Opaque != nullptr) && (Picture != nullptr))
	{
		UE_LOG(LogVlcMedia, VeryVerbose, TEXT("Callbacks %llx: StaticVideoUnlockCallback"), Opaque);
	}

	// discard temporary buffer for VLC crash workaround
	if ((Picture == nullptr) && (Planes != nullptr) && (Planes[0] != nullptr))
	{
		FMemory::Free(Planes[0]);
	}
}
