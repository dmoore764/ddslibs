/*
 * aseprite_importer.cpp - public domain
 * Authored Feb. 2016 by Daniel Moore  www.ddsgames.com  (twitter @indie_dds)
 *
 * This library parses .ase files (the save file format of Aseprite) in a 
 * single pass, and includes an example function that renders a single frame
 * of the file (combining layers based on opacity/blending mode, etc.).
 *
 * Details of the file specification can be found here:
 * https://github.com/aseprite/aseprite/blob/master/docs/files/ase.txt
 *
 * TODO:
 *   Implement remaining blending modes (Hue, Saturation, Color, Luminosity)
 *   Read chunk data (chunks are packets of data stored in the file) other than frame/layer/cel/palette
 *   Parse cel data of the 'linked' type
 *   Implement 16-bit (grayscale) color mode
 *
 * Uses tinfl.c for decompression.  Please obtain this file from:
 * https://code.google.com/archive/p/miniz/source/default/source
 */

#include "tinfl.c"

/*
 * You can turn on the parser's printf output by commenting out the following line
 */

#define ASEPRITE_NO_DEBUG_OUTPUT

/*
 *
 * USAGE:
 * Requires a few c standard library includes
 *  - #include <stdlib.h> (malloc, realloc)
 *  - #include <stdint.h> (for uint16_t, uint8_t, etc.)
 *
 * Example:
 *
 * ...
 * void *FileData = MyLoadFileFunction("example.ase");		//you must implement your own file read function
 * aseprite_file ParsedFile = AsepriteParseFile(FileData);	//parse the file
 * free(FileData);											//ok to free the file data here
 * void *FrameData = AsepriteGetEntireFrameRGBA(&ParsedFile, 0); //get decompressed frame data
 * glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ParsedFile.Header.WidthInPixels, ParsedFile.Header.HeightInPixels, 0, GL_RGBA, GL_UNSIGNED_BYTE, (const GLvoid *)FrameData);
 * free(FrameData);
 * ...
 *
 */

#ifdef ASEPRITE_NO_DEBUG_OUTPUT
#define printf_d printf_nooutput
#else
#define printf_d printf
#endif

static void
printf_nooutput(const char *OutString, ...) {}

#pragma pack(1)

//The following structs are laid out exactly as described in the file spec.
//pragma pack is used to tell the compiler not to insert extra spacing
//around the members of the struct

struct aseprite_header
{
	uint32_t FileSize;
	uint16_t MagicNumber;
	uint16_t Frames;
	uint16_t WidthInPixels;
	uint16_t HeightInPixels;
	uint16_t ColorDepth;
	uint32_t Flags;
	uint16_t Speed;
	uint32_t SpacerA[2];
	uint8_t TransparentPaletteEntry;
	uint8_t SpacerB[3];
	uint16_t NumberOfColors;
	uint8_t SpacerC[94];
};

struct aseprite_frame_header
{
	uint32_t BytesInFrame;
	uint16_t MagicNumber;
	uint16_t ChunksInFrame;
	uint16_t FrameDuration;
	uint8_t Spacer[6];
};

struct aseprite_chunk_header
{
	uint32_t ChunkSize;
	uint16_t ChunkType;
};

struct aseprite_palette_header
{
	uint32_t NewPaletteSize;
	uint32_t FirstColorIndexToChange;
	uint32_t LastColorIndexToChange;
	uint8_t Spacer[8];
};

struct aseprite_palette_entry
{
	uint16_t Flags;
	uint8_t Red;
	uint8_t Green;
	uint8_t Blue;
	uint8_t Alpha;
};

struct aseprite_layer_header
{
	uint16_t Flags;
	uint16_t LayerType;
	uint16_t LayerChild;
	uint16_t DefaultLayerWidthInPixels; //Ignored
	uint16_t DefaultLayerHeightInPixels; //Ignored
	uint16_t BlendMode;
	uint8_t Opacity;
	uint8_t Spacer[3];
};

struct aseprite_cel_header
{
	uint16_t LayerIndex;
	int16_t XPos;
	int16_t YPos;
	uint8_t Opacity;
	uint16_t CelType;
	uint8_t Spacer[7];
};

#pragma options align=reset

// The following structs are things that I defined myself to hold all the relevant
// data for the file.

struct aseprite_layer
{
	aseprite_cel_header Header;
	int DataWidth;
	int DataHeight;
	void *Data;
};

struct aseprite_layer_info
{
	aseprite_layer_header Header;
	char *Name;
};

struct aseprite_color
{
	union
	{
		struct {
			uint8_t R8, G8, B8, A8;
		};
		uint8_t RGBA8[4];
	};
	
	union
	{
		struct {
			float R, G, B, A;
		};
		float RGBA[4];
	};
};

inline aseprite_color
AsepriteColorFromR8G8B8A8(uint8_t R8, uint8_t G8, uint8_t B8, uint8_t A8)
{
	aseprite_color Result;
	Result.R8 = R8;
	Result.G8 = G8;
	Result.B8 = B8;
	Result.A8 = A8;
	Result.R = Result.R8 / 255.0f;
	Result.G = Result.G8 / 255.0f;
	Result.B = Result.B8 / 255.0f;
	Result.A = Result.A8 / 255.0f;
	return Result;
}

inline aseprite_color
AsepriteColorFromRGBA8(uint32_t *RGBA8)
{
	aseprite_color Result;
	*((uint32_t *)Result.RGBA8) = *RGBA8;
	Result.R = Result.R8 / 255.0f;
	Result.G = Result.G8 / 255.0f;
	Result.B = Result.B8 / 255.0f;
	Result.A = Result.A8 / 255.0f;
	return Result;
}

inline aseprite_color
AsepriteColorFromRGBA(float R, float G, float B, float A)
{
	aseprite_color Result;
	Result.R = R;
	Result.G = G;
	Result.B = B;
	Result.A = A;
	Result.R8 = (uint8_t)(R * 255);
	Result.G8 = (uint8_t)(G * 255);
	Result.B8 = (uint8_t)(B * 255);
	Result.A8 = (uint8_t)(A * 255);
	return Result;
}

struct aseprite_palette
{
	aseprite_palette_header Header;
	int NumColors;
	aseprite_color *Colors;
};

struct aseprite_frame
{
	aseprite_frame_header Header;
	int NumLayers;
	aseprite_layer *Layers;
};

struct aseprite_file
{
	aseprite_header Header;
	int NumFrames;
	aseprite_frame *Frames;
	aseprite_palette Palette;
	int NumLayers;
	aseprite_layer_info *LayerInfo;
};

struct aseprite_string
{
	uint16_t Length;
	char *String;
};


//The following are enumerated values as defined in the spec

enum aseprite_chunk_type
{
	AsepriteChunk_OldPalette = 0x0004,
	AsepriteChunk_OldPalette2 = 0x0011,
	AsepriteChunk_Layer = 0x2004,
	AsepriteChunk_Cel = 0x2005,
	AsepriteChunk_Mask = 0x2016,
	AsepriteChunk_Path = 0x2017,
	AsepriteChunk_FrameTags = 0x2018,
	AsepriteChunk_Palette = 0x2019,
	AsepriteChunk_UserData = 0x2020,
};

enum aseprite_layer_flags
{
	AsepriteLayerFlags_Visible = 1,
	AsepriteLayerFlags_Editable = 2,
	AsepriteLayerFlags_LockMovement = 4,
	AsepriteLayerFlags_Background = 8,
	AsepriteLayerFlags_PreferLinkedCels = 16,
};

enum aseprite_blend_mode
{ 
	AsepriteBlendMode_Normal = 0,
    AsepriteBlendMode_Multiply = 1,
    AsepriteBlendMode_Screen   = 2,
    AsepriteBlendMode_Overlay  = 3,
    AsepriteBlendMode_Darken   = 4,
    AsepriteBlendMode_Lighten  = 5,
    AsepriteBlendMode_ColorDodge = 6,
    AsepriteBlendMode_ColorBurn  = 7,
    AsepriteBlendMode_HardLight  = 8,
    AsepriteBlendMode_SoftLight  = 9,
    AsepriteBlendMode_Difference  = 10,
    AsepriteBlendMode_Exclusion   = 11,
    AsepriteBlendMode_Hue         = 12,
    AsepriteBlendMode_Saturation  = 13,
    AsepriteBlendMode_Color       = 14,
    AsepriteBlendMode_Luminosity  = 15,
};

enum aseprite_cel_type
{
	AsepriteCelType_Raw = 0,
	AsepriteCelType_Linked = 1,
	AsepriteCelType_Compressed = 2,
};

// This struct is passed around, and holds the details about where in the file
// we are.  AvailableLayers is used because we allocate storage for a few layers
// to begin with, and increase (realloc) as necessary.  This way we don't have to 
// parse the file multiple times (getting the number of layers first, then going
// back and filling in the data), and it also prevents us from using some sort of
// std::vector kind of nonsense.

struct aseprite_parser
{
	void *At;
	int AvailableLayers;
	bool UsesNewPalette;
};

aseprite_string
AsepriteParseString(void *Data)
{
	aseprite_string Result;
	uint16_t *Length = (uint16_t *)Data;
	Result.Length = *Length;
	Result.String = (char *)((uint16_t *)Data + 1);
	return Result;
};

void
AsepriteParsePalette(aseprite_file *File, void *ChunkData)
{
	aseprite_palette_header *PaletteHeader = (aseprite_palette_header *)ChunkData;
	ChunkData = ((aseprite_palette_header *)ChunkData + 1);

	File->Palette.Header = *PaletteHeader;
	File->Palette.NumColors = PaletteHeader->NewPaletteSize;
	File->Palette.Colors = (aseprite_color *)malloc(sizeof(aseprite_color)*PaletteHeader->NewPaletteSize);

	printf_d("   Num Entries: %d\n", PaletteHeader->NewPaletteSize);
	for (int EntryIndex = PaletteHeader->FirstColorIndexToChange; EntryIndex <= PaletteHeader->LastColorIndexToChange; EntryIndex++)
	{
		aseprite_palette_entry *PaletteEntry = (aseprite_palette_entry *)ChunkData;
		ChunkData = ((aseprite_palette_entry *)ChunkData + 1);
		if (PaletteEntry->Flags == 1)
		{
			aseprite_string ColorName = AsepriteParseString(ChunkData);
			ChunkData = ((char *)ChunkData + sizeof(uint16_t) + ColorName.Length);
			printf_d("     Color name: %.*s\n", ColorName.Length, ColorName.String);
		}
		printf_d("     R%d G%d B%d A%d\n", PaletteEntry->Red, PaletteEntry->Green, PaletteEntry->Blue, PaletteEntry->Alpha);

		aseprite_color *Color = &File->Palette.Colors[EntryIndex];
		*Color = AsepriteColorFromR8G8B8A8(PaletteEntry->Red, PaletteEntry->Green, PaletteEntry->Blue, PaletteEntry->Alpha);
	}
}

void
AsepriteParseOldPalette(aseprite_file *File, void *ChunkData)
{
	uint16_t Packets = *((uint16_t *)ChunkData);
	ChunkData = ((uint16_t *)ChunkData + 1);
	File->Palette.Colors = (aseprite_color *)malloc(sizeof(aseprite_color)*256);

	for (int PacketIndex = 0; PacketIndex < Packets; PacketIndex++)
	{
		uint8_t StartIndex = *((uint8_t *)ChunkData);
		ChunkData = ((uint8_t *)ChunkData + 1);
		uint8_t NumColors = *((uint8_t *)ChunkData);
		ChunkData = ((uint8_t *)ChunkData + 1);
		uint16_t NumColors16 = NumColors;
		if (NumColors == 0)
			NumColors16 = 256;
		for (int ColorIndex = StartIndex; ColorIndex < StartIndex + NumColors16; ColorIndex++)
		{
			uint8_t Red = *((uint8_t *)ChunkData);
			ChunkData = ((uint8_t *)ChunkData + 1);
			uint8_t Green = *((uint8_t *)ChunkData);
			ChunkData = ((uint8_t *)ChunkData + 1);
			uint8_t Blue = *((uint8_t *)ChunkData);
			ChunkData = ((uint8_t *)ChunkData + 1);
			aseprite_color *Color = &File->Palette.Colors[ColorIndex];
			*Color = AsepriteColorFromR8G8B8A8(Red, Green, Blue, 255);
		}
	}
}

void
AsepriteParseCel(aseprite_frame *Frame, void *ChunkData, int ChunkLength)
{
	aseprite_cel_header *CelHeader = (aseprite_cel_header *)ChunkData;
	ChunkData = ((aseprite_cel_header *)ChunkData + 1);

	aseprite_layer *Layer = &Frame->Layers[CelHeader->LayerIndex];
	Layer->Header = *CelHeader;

	printf_d("  Layer Index: %d\n", CelHeader->LayerIndex);
	printf_d("  XPos: %d\n", CelHeader->XPos);
	printf_d("  YPos: %d\n", CelHeader->YPos);
	printf_d("  Opacity: %d\n", CelHeader->Opacity);

	printf_d("  Cel Type:  ");
	switch (CelHeader->CelType)
	{
		case AsepriteCelType_Raw: 
		{
			printf_d("Raw\n");
			uint16_t WidthInPixels = *((uint16_t *)ChunkData);
			ChunkData = ((uint16_t *)ChunkData + 1);
			uint16_t HeightInPixels = *((uint16_t *)ChunkData);
			ChunkData = ((uint16_t *)ChunkData + 1);
			int DataSize = ChunkLength - sizeof(aseprite_cel_header) - sizeof(uint16_t)*2;
			void *Data = ChunkData;
			Layer->DataWidth = WidthInPixels;
			Layer->DataHeight = HeightInPixels;
			Layer->Data = malloc(DataSize);
			memcpy(Layer->Data, ChunkData, DataSize);
		} break;
		case AsepriteCelType_Linked: 
		{
			printf_d("Linked\n");
		} break;
		case AsepriteCelType_Compressed: 
		{
			printf_d("Compressed\n");
			uint16_t WidthInPixels = *((uint16_t *)ChunkData);
			ChunkData = ((uint16_t *)ChunkData + 1);
			uint16_t HeightInPixels = *((uint16_t *)ChunkData);
			ChunkData = ((uint16_t *)ChunkData + 1);
			printf_d("  Cel data size x,y (%d, %d)\n", WidthInPixels, HeightInPixels);
			int DataSize = ChunkLength - sizeof(aseprite_cel_header) - sizeof(uint16_t)*2;
			size_t DataLength = 0;
			void *Data = tinfl_decompress_mem_to_heap(ChunkData, DataSize, &DataLength, TINFL_FLAG_PARSE_ZLIB_HEADER);
			Layer->DataWidth = WidthInPixels;
			Layer->DataHeight = HeightInPixels;
			Layer->Data = Data;
			
			/*
			if (Data)
			{
				uint8_t *Bytes = (uint8_t *)Data;
				for (int Y = 180; Y < HeightInPixels; Y++)
				{
					for (int X = 0; X < WidthInPixels; X++)
					{
						printf_d("%d ", Bytes[X + Y*WidthInPixels]);
					}
					printf_d("\n");
				}
			}*/
			printf_d("\n");
			
		} break;
	}
}

void
AsepriteParseLayer(aseprite_file *File, aseprite_parser *Parser, void *ChunkData)
{
	if (File->NumLayers == Parser->AvailableLayers)
	{
		Parser->AvailableLayers *= 2;
		File->LayerInfo = (aseprite_layer_info *)realloc(File->LayerInfo, sizeof(aseprite_layer_info)*Parser->AvailableLayers);
	}

	aseprite_layer_info *NewLayer = &File->LayerInfo[File->NumLayers++];

	aseprite_layer_header *LayerHeader = (aseprite_layer_header *)ChunkData;
	ChunkData = ((aseprite_layer_header *)ChunkData + 1);

	NewLayer->Header = *LayerHeader;

	printf_d(" Layer Flags\n");
	if (LayerHeader->Flags & AsepriteLayerFlags_Visible)
		printf_d("     Visible\n");
	if (LayerHeader->Flags & AsepriteLayerFlags_Editable)
		printf_d("     Editable\n");
	if (LayerHeader->Flags & AsepriteLayerFlags_LockMovement)
		printf_d("     Lock Movement\n");
	if (LayerHeader->Flags & AsepriteLayerFlags_Background)
		printf_d("     Background\n");
	if (LayerHeader->Flags & AsepriteLayerFlags_PreferLinkedCels)
		printf_d("     Prefer Linked Cels\n");

	printf_d(" Layer Type: %d\n", LayerHeader->LayerType);
	printf_d(" Layer Child: %d\n", LayerHeader->LayerChild);

	printf_d(" Blend Mode:  ");
	switch (LayerHeader->BlendMode)
	{
		case AsepriteBlendMode_Normal: {printf_d("Normal\n");} break;
		case AsepriteBlendMode_Multiply: {printf_d("Multiply\n");} break;
		case AsepriteBlendMode_Screen: {printf_d("Screen \n");} break;
		case AsepriteBlendMode_Overlay: {printf_d("Overlay \n");} break;
		case AsepriteBlendMode_Darken: {printf_d("Darken \n");} break;
		case AsepriteBlendMode_Lighten: {printf_d("Lighten \n");} break;
		case AsepriteBlendMode_ColorDodge: {printf_d("Color Dodge\n");} break;
		case AsepriteBlendMode_ColorBurn: {printf_d("Color Burn\n");} break;
		case AsepriteBlendMode_HardLight: {printf_d("Hard Light\n");} break;
		case AsepriteBlendMode_SoftLight: {printf_d("Soft Light\n");} break;
		case AsepriteBlendMode_Difference: {printf_d("Difference\n");} break;
		case AsepriteBlendMode_Exclusion: {printf_d("Exclusion\n");} break;
		case AsepriteBlendMode_Hue: {printf_d("Hue \n");} break;
		case AsepriteBlendMode_Saturation: {printf_d("Saturation\n");} break;
		case AsepriteBlendMode_Color: {printf_d("Color\n");} break;
		case AsepriteBlendMode_Luminosity: {printf_d("Luminosity\n");} break;
	}

	printf_d(" Opacity: %d\n", LayerHeader->Opacity);
	aseprite_string LayerName = AsepriteParseString(ChunkData);
	printf_d(" Layer name: %.*s\n", LayerName.Length, LayerName.String);

	NewLayer->Name = (char *)malloc(LayerName.Length + 1);
	memcpy(NewLayer->Name, LayerName.String, LayerName.Length);
	NewLayer->Name[LayerName.Length] = '\0';
}

// You can see which 'chunk' types are not implemented here.  Some are deprecated, so no
// need to fill those in.

void
AsepriteParseChunk(aseprite_file *File, aseprite_frame *Frame, aseprite_parser *Parser)
{
	aseprite_chunk_header *ChunkHeader = (aseprite_chunk_header *)Parser->At;
	void *ChunkData = ((aseprite_chunk_header *)Parser->At + 1);
	Parser->At = ((char *)Parser->At + ChunkHeader->ChunkSize);

	printf_d("Chunk type: ");
	switch (ChunkHeader->ChunkType)
	{
		case AsepriteChunk_OldPalette:
		{
			printf_d("old palette\n");
			if (!Parser->UsesNewPalette)
				AsepriteParseOldPalette(File, ChunkData);
		} break;
		case AsepriteChunk_OldPalette2:
		{
			printf_d("old palette\n");
		} break;
		case AsepriteChunk_Layer:
		{
			printf_d("layer\n");
			AsepriteParseLayer(File, Parser, ChunkData);
		} break;
		case AsepriteChunk_Cel:
		{
			printf_d("cel\n");
			if (Frame->NumLayers != File->NumLayers)
			{
				Frame->NumLayers = File->NumLayers;
				Frame->Layers = (aseprite_layer *)malloc(sizeof(aseprite_layer)*Frame->NumLayers);
			}
			AsepriteParseCel(Frame, ChunkData, ChunkHeader->ChunkSize - sizeof(aseprite_chunk_header));
		} break;
		case AsepriteChunk_Mask:
		{
			printf_d("mask\n");
		} break;
		case AsepriteChunk_Path:
		{
			printf_d("path\n");
		} break;
		case AsepriteChunk_FrameTags:
		{
			printf_d("frame tags\n");
		} break;
		case AsepriteChunk_Palette:
		{
			printf_d("palette\n");
			Parser->UsesNewPalette = true;
			AsepriteParsePalette(File, ChunkData);
		} break;
		case AsepriteChunk_UserData:
		{
			printf_d("user data\n");
		} break;
	}
	printf_d("\n");
}

void
AsepriteParseFrame(aseprite_file *File, aseprite_frame *Frame, aseprite_parser *Parser)
{
	aseprite_frame_header *FrameHeader = (aseprite_frame_header *)Parser->At;
	Parser->At = ((aseprite_frame_header *)Parser->At + 1);

	Frame->Header = *FrameHeader;

	printf_d("Frame: \n");
	printf_d("   Duration: %d\n", FrameHeader->FrameDuration);
	printf_d("CHUNKS: \n");

	for (int ChunkIndex = 0; ChunkIndex < FrameHeader->ChunksInFrame; ChunkIndex++)
	{
		AsepriteParseChunk(File, Frame, Parser);
	}

	printf_d("\n");
}

aseprite_file
AsepriteParseFile(void *FileData)
{
	aseprite_file Result;

	aseprite_parser Parser = {0};
	Parser.At = FileData;
	Parser.AvailableLayers = 2;

	aseprite_header *Header = (aseprite_header *)Parser.At;
	Parser.At = ((aseprite_header *)Parser.At + 1);

	Result.Header = *Header;
	Result.NumFrames = Header->Frames;
	Result.Frames = (aseprite_frame *)malloc(sizeof(aseprite_frame)*Header->Frames);
	Result.NumLayers = 0;
	Result.LayerInfo = (aseprite_layer_info *)malloc(sizeof(aseprite_layer_info)*Parser.AvailableLayers);

	for (int FrameIndex = 0; FrameIndex < Header->Frames; FrameIndex++)
	{
		AsepriteParseFrame(&Result, &Result.Frames[FrameIndex], &Parser);
	}

	return Result;

}

inline float
AsepriteMin(float A, float B)
{
	float Result = A;
	if (B < A)
		Result = B;
	return Result;
}

inline float
AsepriteAbs(float A)
{
	float Result = A;
	if (A < 0)
		Result = -A;
	return Result;
}

inline aseprite_color
AsepriteCombineColors(aseprite_color *Src, aseprite_color *Dest, aseprite_blend_mode BlendMode)
{
	aseprite_color Result;

	float OutAlpha = Src->A + Dest->A*(1 - Src->A);
	if (OutAlpha == 0)
	{
		Result = AsepriteColorFromRGBA(0,0,0,0);
	} 
	else
	{
		float OutRed, OutGreen, OutBlue;
		switch (BlendMode)
		{
			case AsepriteBlendMode_Multiply:
			{
				OutRed = Src->R*Dest->R;
				OutGreen = Src->G*Dest->G;
				OutBlue = Src->B*Dest->B;
			} break;
			case AsepriteBlendMode_Screen:
			{
				OutRed = 1 - (1 - Src->R)*(1 - Dest->R);
				OutGreen = 1 - (1 - Src->G)*(1 - Dest->G);
				OutBlue = 1 - (1 - Src->B)*(1 - Dest->B);
			} break;
			case AsepriteBlendMode_Overlay:
			{
				OutRed = (Dest->R < 0.5f) ? (2*Src->R*Dest->R) : (1 - 2*(1 - Src->R)*(1 - Dest->R));
				OutGreen = (Dest->G < 0.5f) ? (2*Src->G*Dest->G) : (1 - 2*(1 - Src->G)*(1 - Dest->G));
				OutBlue = (Dest->B < 0.5f) ? (2*Src->B*Dest->B) : (1 - 2*(1 - Src->B)*(1 - Dest->B));
			} break;
			case AsepriteBlendMode_Darken:
			{
				OutRed = (Dest->R < Src->R) ? (Dest->R) : (Src->R);
				OutGreen = (Dest->G < Src->G) ? (Dest->G) : (Src->G);
				OutBlue = (Dest->B < Src->B) ? (Dest->B) : (Src->B);
			} break;
			case AsepriteBlendMode_Lighten:
			{
				OutRed = (Dest->R > Src->R) ? (Dest->R) : (Src->R);
				OutGreen = (Dest->G > Src->G) ? (Dest->G) : (Src->G);
				OutBlue = (Dest->B > Src->B) ? (Dest->B) : (Src->B);
			} break;
			case AsepriteBlendMode_ColorDodge:
			{
				OutRed = (Src->R == 1) ? (1) : (Dest->R / (1 - Src->R));
				OutRed = AsepriteMin(OutRed, 1);
				OutGreen = (Src->G == 1) ? (1) : (Dest->G / (1 - Src->G));
				OutGreen = AsepriteMin(OutGreen, 1);
				OutBlue = (Src->B == 1) ? (1) : (Dest->B / (1 - Src->B));
				OutBlue = AsepriteMin(OutBlue, 1);
			} break;
			case AsepriteBlendMode_ColorBurn:
			{
				OutRed = (Src->R == 0) ? 0 : (1 - AsepriteMin((1 - Dest->R)/(Src->R), 1));
				OutGreen = (Src->G == 0) ? 0 : (1 - AsepriteMin((1 - Dest->G)/(Src->G), 1));
				OutBlue = (Src->B == 0) ? 0 : (1 - AsepriteMin((1 - Dest->B)/(Src->B), 1));
			} break;
			case AsepriteBlendMode_HardLight:
			{
				OutRed = (Src->R < 0.5f) ? (2*Src->R*Dest->R) : (1 - 2*(1 - Src->R)*(1 - Dest->R));
				OutGreen = (Src->G < 0.5f) ? (2*Src->G*Dest->G) : (1 - 2*(1 - Src->G)*(1 - Dest->G));
				OutBlue = (Src->B < 0.5f) ? (2*Src->B*Dest->B) : (1 - 2*(1 - Src->B)*(1 - Dest->B));
			} break;
			case AsepriteBlendMode_SoftLight:
			{
				OutRed = (1 - 2*Src->R)*Dest->R*Dest->R + 2*Dest->R*Src->R;
				OutGreen = (1 - 2*Src->G)*Dest->G*Dest->G + 2*Dest->G*Src->G;
				OutBlue = (1 - 2*Src->B)*Dest->B*Dest->B + 2*Dest->B*Src->B;
			} break;
			case AsepriteBlendMode_Difference:
			{
				OutRed = AsepriteAbs(Dest->R - Src->R);
				OutGreen = AsepriteAbs(Dest->G - Src->G);
				OutBlue = AsepriteAbs(Dest->B - Src->B);
			} break;
			case AsepriteBlendMode_Exclusion:
			{
				OutRed = 0.5f - 2*(Dest->R - 0.5f)*(Src->R - 0.5f);
				OutGreen = 0.5f - 2*(Dest->G - 0.5f)*(Src->G - 0.5f);
				OutBlue = 0.5f - 2*(Dest->B - 0.5f)*(Src->B - 0.5f);
			} break;
			default:
			{
				//Defaults to normal blend mode
				OutRed = Src->R;
				OutGreen = Src->G;
				OutBlue = Src->B;
			} break;
		}
		//Alpha compositing
		float OneOverOutAlpha = 1.0f / OutAlpha;
		OutRed = (OutRed*Src->A + Dest->R*Dest->A*(1-Src->A)) * OneOverOutAlpha;
		OutGreen = (OutGreen*Src->A + Dest->G*Dest->A*(1-Src->A)) * OneOverOutAlpha;
		OutBlue = (OutBlue*Src->A + Dest->B*Dest->A*(1-Src->A)) * OneOverOutAlpha;
		Result = AsepriteColorFromRGBA(OutRed, OutGreen, OutBlue, OutAlpha);
	}

	return Result;
}

void
AsepriteGetEntireFrameRGBA(aseprite_file *File, int FrameNumber, void *DestTexture, int DestWidth, int DestHeight, int DestX, int DestY)
{
	Assert(FrameNumber < File->NumFrames);
	
	int Width = File->Header.WidthInPixels;
	int Height = File->Header.HeightInPixels;
	aseprite_frame *Frame = File->Frames + FrameNumber;
	aseprite_palette *Palette = &File->Palette;
	uint8_t TransparentPaletteEntry = File->Header.TransparentPaletteEntry;

	for (int LayerIndex = 0; LayerIndex < File->NumLayers; LayerIndex++)
	{
		aseprite_layer_info *LayerInfo = File->LayerInfo + LayerIndex;
		if (LayerInfo->Header.Opacity == 0 || (LayerInfo->Header.Flags & AsepriteLayerFlags_Visible) == 0)
			continue;

		aseprite_blend_mode BlendMode = (aseprite_blend_mode)LayerInfo->Header.BlendMode;
		aseprite_layer *Layer = Frame->Layers + LayerIndex;
		void *CelData = Layer->Data;
		uint16_t ColorDepth = File->Header.ColorDepth;
		float LayerOpacity = LayerInfo->Header.Opacity / 255.0f;

		int DestPitch = DestWidth*4;
		int DestStartX = (DestX < 0) ? 0 : (DestX*4);

		int DataPitch = Layer->DataWidth;
		if (ColorDepth == 32)
			DataPitch *= 4;

		int StartY = 0 - Layer->Header.YPos;
		int StartX = 0 - Layer->Header.XPos;
		if (ColorDepth == 32)
			StartX *= 4;

		for (int Y = 0; Y < Height; Y++)
		{
			if (Y + DestY < 0)
				continue;
			if (Y + DestY >= DestHeight)
				break;

			uint32_t *Dest = (uint32_t *)((uint8_t *)DestTexture + (DestY + Y)*DestPitch + DestStartX);
			uint8_t *Source = ((uint8_t *)CelData + (StartY + Y)*DataPitch + StartX);
			for (int X = 0; X < Width; X++)
			{
				if (X + DestX < 0)
					continue;
				if (X + DestX >= DestWidth)
					break;

				aseprite_color SourceColor;
				switch (ColorDepth)
				{
					case 8:
					{
						uint8_t PaletteIndex = (*Source++);
						if (PaletteIndex == TransparentPaletteEntry)
							SourceColor = (aseprite_color){0};
						else
							SourceColor = Palette->Colors[PaletteIndex];
					} break;
					case 32:
					{
						uint32_t *Src = (uint32_t *)Source;
						Source = (uint8_t *)(Src + 1);
						SourceColor = AsepriteColorFromRGBA8(Src);
					} break;
				}
				if (LayerOpacity == 0)
				{
					SourceColor = (aseprite_color){0};
				}
				else if (LayerOpacity != 1)
				{
					SourceColor.A *= LayerOpacity;
					SourceColor.A8 = (uint8_t)(SourceColor.A * 255);
				}
				if (LayerIndex == 0 || *Dest == 0)
					*Dest = *((uint32_t *)SourceColor.RGBA8);
				else if (*SourceColor.RGBA8 != 0)
				{
					aseprite_color DestColor = AsepriteColorFromRGBA8(Dest);
					aseprite_color FinalColor = AsepriteCombineColors(&SourceColor, &DestColor, BlendMode);
					*Dest = *((uint32_t *)FinalColor.RGBA8);
				}
				Dest++;
			}
		}
	}
}
