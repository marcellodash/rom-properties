/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * WiiWAD.cpp: Nintendo Wii WAD file reader.                               *
 *                                                                         *
 * Copyright (c) 2016-2018 by David Korth.                                 *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "WiiWAD.hpp"
#include "librpbase/RomData_p.hpp"

#include "gcn_structs.h"
#include "wii_structs.h"
#include "wii_wad.h"
#include "wii_banner.h"
#include "data/NintendoLanguage.hpp"
#include "data/WiiSystemMenuVersion.hpp"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/SystemRegion.hpp"
#include "librpbase/file/IRpFile.hpp"

#include "libi18n/i18n.h"
using namespace LibRpBase;

// Decryption.
#include "librpbase/crypto/KeyManager.hpp"
#ifdef ENABLE_DECRYPTION
# include "librpbase/crypto/AesCipherFactory.hpp"
# include "librpbase/crypto/IAesCipher.hpp"
# include "librpbase/disc/CBCReader.hpp"
// Key verification.
# include "disc/WiiPartition.hpp"
#endif /* ENABLE_DECRYPTION */

// C includes. (C++ namespace)
#include "librpbase/ctypex.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

// C++ includes.
#include <sstream>
#include <string>
#include <vector>
using std::string;
using std::vector;

namespace LibRomData {

ROMDATA_IMPL(WiiWAD)
ROMDATA_IMPL_IMG(WiiWAD)

class WiiWADPrivate : public RomDataPrivate
{
	public:
		WiiWADPrivate(WiiWAD *q, IRpFile *file);
		~WiiWADPrivate();

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(WiiWADPrivate)

	public:
		// WAD structs.
		Wii_WAD_Header wadHeader;
		RVL_Ticket ticket;
		RVL_TMD_Header tmdHeader;

		/**
		 * Round a value to the next highest multiple of 64.
		 * @param value Value.
		 * @return Next highest multiple of 64.
		 */
		template<typename T>
		static inline T toNext64(T val)
		{
			return (val + (T)63) & ~((T)63);
		}

#ifdef ENABLE_DECRYPTION
		// CBC reader for the main data area.
		CBCReader *cbcReader;

		// Main data headers.
		Wii_Content_Bin_Header contentHeader;
		Wii_IMET_t imet;	// NOTE: May be WIBN.
#endif /* ENABLE_DECRYPTION */
		// Key index.
		WiiPartition::EncryptionKeys key_idx;
		// Key status.
		KeyManager::VerifyResult key_status;

		/**
		 * Get the game information string from the banner.
		 * @return Game information string, or empty string on error.
		 */
		string getGameInfo(void);

		/**
		 * Convert a Wii WAD region value to a GameTDB region code.
		 * @param idRegion Game ID region.
		 *
		 * NOTE: Mulitple GameTDB region codes may be returned, including:
		 * - User-specified fallback region. [TODO]
		 * - General fallback region.
		 *
		 * TODO: Get the actual Wii region value from the WAD file.
		 *
		 * @return GameTDB region code(s), or empty vector if the region value is invalid.
		 */
		static vector<const char*> wadRegionToGameTDB(char idRegion);
};

/** WiiWADPrivate **/

WiiWADPrivate::WiiWADPrivate(WiiWAD *q, IRpFile *file)
	: super(q, file)
#ifdef ENABLE_DECRYPTION
	, cbcReader(nullptr)
#endif /* ENABLE_DECRYPTION */
	, key_idx(WiiPartition::Key_Max)
	, key_status(KeyManager::VERIFY_UNKNOWN)
{
	// Clear the various structs.
	memset(&wadHeader, 0, sizeof(wadHeader));
	memset(&ticket, 0, sizeof(ticket));
	memset(&tmdHeader, 0, sizeof(tmdHeader));

#ifdef ENABLE_DECRYPTION
	memset(&contentHeader, 0, sizeof(contentHeader));
	memset(&imet, 0, sizeof(imet));
#endif /* ENABLE_DECRYPTION */
}

WiiWADPrivate::~WiiWADPrivate()
{
#ifdef ENABLE_DECRYPTION
	delete cbcReader;
#endif /* ENABLE_DECRYPTION */
}

/**
 * Get the game information string from the banner.
 * @return Game information string, or empty string on error.
 */
string WiiWADPrivate::getGameInfo(void)
{
#ifdef ENABLE_DECRYPTION
	// IMET header.
	// TODO: Read on demand instead of always reading in the constructor.
	if (imet.magic != cpu_to_be32(WII_IMET_MAGIC)) {
		// Not valid.
		return string();
	}

	// TODO: Combine with GameCubePrivate::wii_getBannerName()?

	// Get the system language.
	// TODO: Verify against the region code somehow?
	int lang = NintendoLanguage::getWiiLanguage();

	// If the language-specific name is empty,
	// revert to English.
	if (imet.names[lang][0][0] == 0) {
		// Revert to English.
		lang = WII_LANG_ENGLISH;
	}

	// NOTE: The banner may have two lines.
	// Each line is a maximum of 21 characters.
	// Convert from UTF-16 BE and split into two lines at the same time.
	string info = utf16be_to_utf8(imet.names[lang][0], 21);
	if (imet.names[lang][1][0] != 0) {
		info += '\n';
		info += utf16be_to_utf8(imet.names[lang][1], 21);
	}

	return info;
#else /* !ENABLE_DECRYPTION */
	// Unable to decrypt the IMET header.
	return string();
#endif /* ENABLE_DECRYPTION */
}

/**
 * Convert a Wii WAD region value to a GameTDB region code.
 * @param idRegion Game ID region.
 *
 * NOTE: Mulitple GameTDB region codes may be returned, including:
 * - User-specified fallback region. [TODO]
 * - General fallback region.
 *
 * TODO: Get the actual Wii region value from the WAD file.
 *
 * @return GameTDB region code(s), or empty vector if the region value is invalid.
 */
vector<const char*> WiiWADPrivate::wadRegionToGameTDB(char idRegion)
{
	// TODO: Get the actual Wii region value from the WAD file.
	// Using the Game ID region only for now.
	vector<const char*> ret;
	int fallback_region = 0;

	// Check for region-specific game IDs.
	switch (idRegion) {
		case 'E':	// USA
			ret.push_back("US");
			break;
		case 'J':	// Japan
			ret.push_back("JA");
			break;
		case 'O':
			// TODO: US/EU.
			// Compare to host system region.
			// For now, assuming US.
			ret.push_back("US");
			break;
		case 'P':	// PAL
		case 'X':	// Multi-language release
		case 'Y':	// Multi-language release
		case 'L':	// Japanese import to PAL regions
		case 'M':	// Japanese import to PAL regions
		default:
			if (fallback_region == 0) {
				// Use the fallback region.
				fallback_region = 1;
			}
			break;

		// European regions.
		case 'D':	// Germany
			ret.push_back("DE");
			break;
		case 'F':	// France
			ret.push_back("FR");
			break;
		case 'H':	// Netherlands
			ret.push_back("NL");
			break;
		case 'I':	// Italy
			ret.push_back("NL");
			break;
		case 'R':	// Russia
			ret.push_back("RU");
			break;
		case 'S':	// Spain
			ret.push_back("ES");
			break;
		case 'U':	// Australia
			if (fallback_region == 0) {
				// Use the fallback region.
				fallback_region = 2;
			}
			break;
	}

	// Check for fallbacks.
	switch (fallback_region) {
		case 1:
			// Europe
			ret.push_back("EN");
			break;
		case 2:
			// Australia
			ret.push_back("AU");
			ret.push_back("EN");
			break;

		case 0:	// None
		default:
			break;
	}

	return ret;
}

/** WiiWAD **/

/**
 * Read a Nintendo Wii WAD file.
 *
 * A WAD file must be opened by the caller. The file handle
 * will be dup()'d and must be kept open in order to load
 * data from the WAD file.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open disc image.
 */
WiiWAD::WiiWAD(IRpFile *file)
	: super(new WiiWADPrivate(this, file))
{
	// This class handles application packages.
	RP_D(WiiWAD);
	d->className = "WiiWAD";
	d->fileType = FTYPE_APPLICATION_PACKAGE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the WAD header.
	d->file->rewind();
	size_t size = d->file->read(&d->wadHeader, sizeof(d->wadHeader));
	if (size != sizeof(d->wadHeader)) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Check if this WAD file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(d->wadHeader);
	info.header.pData = reinterpret_cast<const uint8_t*>(&d->wadHeader);
	info.ext = nullptr;	// Not needed for WiiWAD.
	info.szFile = d->file->size();
	d->isValid = (isRomSupported_static(&info) >= 0);
	if (!d->isValid) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Read the ticket and TMD.
	// TODO: Verify ticket/TMD sizes.
	unsigned int addr = WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.header_size)) +
			    WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.cert_chain_size));
	size = d->file->seekAndRead(addr, &d->ticket, sizeof(d->ticket));
	if (size != sizeof(d->ticket)) {
		// Seek and/or read error.
		d->isValid = false;
		delete d->file;
		d->file = nullptr;
		return;
	}
	addr += WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.ticket_size));
	size = d->file->seekAndRead(addr, &d->tmdHeader, sizeof(d->tmdHeader));
	if (size != sizeof(d->tmdHeader)) {
		// Seek and/or read error.
		d->isValid = false;
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Determine the key index and debug vs. retail.
	static const char issuer_rvt[] = "Root-CA00000002-XS00000006";
	if (!memcmp(d->ticket.signature_issuer, issuer_rvt, sizeof(issuer_rvt))) {
		// Debug encryption.
		d->key_idx = WiiPartition::Key_Rvt_Debug;
	} else {
		// Retail encryption.
		uint8_t idx = d->ticket.common_key_index;
		if (idx > 2) {
			// Out of range. Assume Wii common key.
			idx = 0;
		}
		d->key_idx = (WiiPartition::EncryptionKeys)idx;
	}

#ifdef ENABLE_DECRYPTION
	// Initialize the CBC reader for the main data area.

	// TODO: WiiVerifyKeys class.
	KeyManager *const keyManager = KeyManager::instance();
	assert(keyManager != nullptr);

	// Key verification data.
	// TODO: Move out of WiiPartition and into WiiVerifyKeys?
	const char *const keyName = WiiPartition::encryptionKeyName_static(d->key_idx);
	const uint8_t *const verifyData = WiiPartition::encryptionVerifyData_static(d->key_idx);
	assert(keyName != nullptr);
	assert(keyName[0] != '\0');
	assert(verifyData != nullptr);

	// Get and verify the key.
	KeyManager::KeyData_t keyData;
	d->key_status = keyManager->getAndVerify(keyName, &keyData, verifyData, 16);
	if (d->key_status != KeyManager::VERIFY_OK) {
		// Unable to get and verify the key.
		return;
	}

	// Create a cipher to decrypt the title key.
	IAesCipher *cipher = AesCipherFactory::create();

	// Initialize parameters for title key decryption.
	// TODO: Error checking.
	// Parameters:
	// - Chaining mode: CBC
	// - IV: Title ID (little-endian)
	cipher->setChainingMode(IAesCipher::CM_CBC);
	cipher->setKey(keyData.key, keyData.length);
	// Title key IV: High 8 bytes are the title ID (in big-endian), low 8 bytes are 0.
	uint8_t iv[16];
	memcpy(iv, &d->ticket.title_id.id, sizeof(d->ticket.title_id.id));
	memset(&iv[8], 0, 8);
	cipher->setIV(iv, sizeof(iv));
	
	// Decrypt the title key.
	uint8_t title_key[16];
	memcpy(title_key, d->ticket.enc_title_key, sizeof(d->ticket.enc_title_key));
	cipher->decrypt(title_key, sizeof(title_key));
	delete cipher;

	// Data area IV:
	// - First two bytes are the big-endian content index.
	// - Remaining bytes are zero.
	// - TODO: Read the TMD content table. For now, assuming index 0.
	memset(iv, 0, sizeof(iv));

	// Create a CBC reader to decrypt the data section.
	// TODO: Verify some known data?
	addr += WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.tmd_size));
	d->cbcReader = new CBCReader(d->file, addr, be32_to_cpu(d->wadHeader.data_size), title_key, iv);

	// Read the content header.
	// NOTE: Continuing even if this fails, since we can show
	// other information from the ticket and TMD.
	size = d->cbcReader->read(&d->contentHeader, sizeof(d->contentHeader));
	if (size == sizeof(d->contentHeader)) {
		// Contents may be one of the following:
		// - IMET header: Most common.
		// - WIBN header: DLC titles.
		size = d->cbcReader->read(&d->imet, sizeof(d->imet));
		if (size == sizeof(d->imet)) {
			// TODO: Use the WiiWIBN subclass.
			// TODO: Create a WiiIMET subclass? (and also use it in GameCube)
			if (d->imet.magic == cpu_to_be32(WII_IMET_MAGIC)) {
				// This is an IMET header.
				// TODO: Do something here?
			}
		}
	}
#else /* !ENABLE_DECRYPTION */
	// Cannot decrypt anything...
	d->key_status = KeyManager::VERIFY_NO_SUPPORT;
#endif /* ENABLE_DECRYPTION */
}

/**
 * Close the opened file.
 */
void WiiWAD::close(void)
{
#ifdef ENABLE_DECRYPTION
	// Close the CBCReader.
	RP_D(WiiWAD);
	delete d->cbcReader;
	d->cbcReader = nullptr;
#endif /* ENABLE_DECRYPTION */

	// Call the superclass function.
	super::close();
}

/** ROM detection functions. **/

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int WiiWAD::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < sizeof(Wii_WAD_Header))
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	// Check for the correct header fields.
	const Wii_WAD_Header *wadHeader = reinterpret_cast<const Wii_WAD_Header*>(info->header.pData);
	if (wadHeader->header_size != cpu_to_be32(sizeof(*wadHeader))) {
		// WAD header size is incorrect.
		return -1;
	}

	// Check WAD type.
	if (wadHeader->type != cpu_to_be32(WII_WAD_TYPE_Is) &&
	    wadHeader->type != cpu_to_be32(WII_WAD_TYPE_ib) &&
	    wadHeader->type != cpu_to_be32(WII_WAD_TYPE_Bk))
	{
		// WAD type is incorrect.
		return -1;
	}

	// Verify the ticket size.
	// TODO: Also the TMD size.
	if (be32_to_cpu(wadHeader->ticket_size) < sizeof(RVL_Ticket)) {
		// Ticket is too small.
		return -1;
	}
	
	// Check the file size to ensure we have at least the IMET section.
	unsigned int expected_size = WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->header_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->cert_chain_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->ticket_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->tmd_size)) +
				     sizeof(Wii_Content_Bin_Header);
	if (expected_size > info->szFile) {
		// File is too small.
		return -1;
	}

	// This appears to be a Wii WAD file.
	return 0;
}

/**
 * Get the name of the system the loaded ROM is designed for.
 * @param type System name type. (See the SystemName enum.)
 * @return System name, or nullptr if type is invalid.
 */
const char *WiiWAD::systemName(unsigned int type) const
{
	RP_D(const WiiWAD);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// Wii has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"WiiWAD::systemName() array index optimization needs to be updated.");

	static const char *const sysNames[4] = {
		"Nintendo Wii", "Wii", "Wii", nullptr
	};

	return sysNames[type & SYSNAME_TYPE_MASK];
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions do not include the leading dot,
 * e.g. "bin" instead of ".bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *WiiWAD::supportedFileExtensions_static(void)
{
	static const char *const exts[] = {
		".wad",

		nullptr
	};
	return exts;
}

/**
 * Get a list of all supported MIME types.
 * This is to be used for metadata extractors that
 * must indicate which MIME types they support.
 *
 * NOTE: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *WiiWAD::supportedMimeTypes_static(void)
{
	static const char *const mimeTypes[] = {
		// Unofficial MIME types from FreeDesktop.org.
		"application/x-wii-wad",

		nullptr
	};
	return mimeTypes;
}

/**
 * Get a bitfield of image types this class can retrieve.
 * @return Bitfield of supported image types. (ImageTypesBF)
 */
uint32_t WiiWAD::supportedImageTypes_static(void)
{
	return IMGBF_EXT_COVER | IMGBF_EXT_COVER_3D |
	       IMGBF_EXT_COVER_FULL |
	       IMGBF_EXT_TITLE_SCREEN;
}

/**
 * Get a list of all available image sizes for the specified image type.
 *
 * The first item in the returned vector is the "default" size.
 * If the width/height is 0, then an image exists, but the size is unknown.
 *
 * @param imageType Image type.
 * @return Vector of available image sizes, or empty vector if no images are available.
 */
std::vector<RomData::ImageSizeDef> WiiWAD::supportedImageSizes_static(ImageType imageType)
{
	assert(imageType >= IMG_INT_MIN && imageType <= IMG_EXT_MAX);
	if (imageType < IMG_INT_MIN || imageType > IMG_EXT_MAX) {
		// ImageType is out of range.
		return std::vector<ImageSizeDef>();
	}

	switch (imageType) {
		case IMG_EXT_COVER: {
			static const ImageSizeDef sz_EXT_COVER[] = {
				{nullptr, 160, 224, 0},
			};
			return vector<ImageSizeDef>(sz_EXT_COVER,
				sz_EXT_COVER + ARRAY_SIZE(sz_EXT_COVER));
		}
		case IMG_EXT_COVER_3D: {
			static const ImageSizeDef sz_EXT_COVER_3D[] = {
				{nullptr, 176, 248, 0},
			};
			return vector<ImageSizeDef>(sz_EXT_COVER_3D,
				sz_EXT_COVER_3D + ARRAY_SIZE(sz_EXT_COVER_3D));
		}
		case IMG_EXT_COVER_FULL: {
			static const ImageSizeDef sz_EXT_COVER_FULL[] = {
				{nullptr, 512, 340, 0},
				{"HQ", 1024, 680, 1},
			};
			return vector<ImageSizeDef>(sz_EXT_COVER_FULL,
				sz_EXT_COVER_FULL + ARRAY_SIZE(sz_EXT_COVER_FULL));
		}
		case IMG_EXT_TITLE_SCREEN: {
			static const ImageSizeDef sz_EXT_TITLE_SCREEN[] = {
				{nullptr, 192, 112, 0},
			};
			return vector<ImageSizeDef>(sz_EXT_TITLE_SCREEN,
				sz_EXT_TITLE_SCREEN + ARRAY_SIZE(sz_EXT_TITLE_SCREEN));
		}
		default:
			break;
	}

	// Unsupported image type.
	return std::vector<ImageSizeDef>();
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int WiiWAD::loadFieldData(void)
{
	RP_D(WiiWAD);
	if (!d->fields->empty()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file || !d->file->isOpen()) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// WAD headers are read in the constructor.
	const RVL_TMD_Header *const tmdHeader = &d->tmdHeader;
	d->fields->reserve(7);	// Maximum of 7 fields.

	if (d->key_status != KeyManager::VERIFY_OK) {
		// Unable to get the decryption key.
		const char *err = KeyManager::verifyResultToString(d->key_status);
		if (!err) {
			err = C_("WiiWAD", "Unknown error. (THIS IS A BUG!)");
		}
		d->fields->addField_string(C_("WiiWAD", "Warning"),
			err, RomFields::STRF_WARNING);
	}

	// Title ID.
	// TODO: Make sure the ticket title ID matches the TMD title ID.
	d->fields->addField_string(C_("WiiWAD", "Title ID"),
		rp_sprintf("%08X-%08X", be32_to_cpu(tmdHeader->title_id.hi), be32_to_cpu(tmdHeader->title_id.lo)));

	// Game ID.
	// NOTE: Only displayed if TID lo is all alphanumeric characters.
	// TODO: Only for certain TID hi?
	if (isalnum(tmdHeader->title_id.u8[4]) &&
	    isalnum(tmdHeader->title_id.u8[5]) &&
	    isalnum(tmdHeader->title_id.u8[6]) &&
	    isalnum(tmdHeader->title_id.u8[7]))
	{
		// Print the game ID.
		// TODO: Is the publisher code available anywhere?
		d->fields->addField_string(C_("WiiWAD", "Game ID"),
			rp_sprintf("%.4s", reinterpret_cast<const char*>(&tmdHeader->title_id.u8[4])));
	}

	// Title version.
	const uint16_t title_version = be16_to_cpu(tmdHeader->title_version);
	d->fields->addField_string(C_("WiiWAD", "Title Version"),
		rp_sprintf("%u.%u (v%u)", title_version >> 8, title_version & 0xFF, title_version));

	// Region code.
	// TODO: Is there a real way to get this from the WAD contents?
	char region_char;
	if (tmdHeader->title_id.hi == cpu_to_be32(0x00000001)) {
		// IOS and/or System Menu.
		if (tmdHeader->title_id.lo == cpu_to_be32(0x00000002)) {
			// System Menu.
			const char *ver = WiiSystemMenuVersion::lookup(title_version);
			region_char = (ver ? ver[3] : '\0');
		} else {
			// IOS, BC, or MIOS. No region.
			region_char = '\0';
		}
	} else {
		// Assume the 4th character of the ID4 is the region code.
		region_char = (char)tmdHeader->title_id.u8[7];
	}

	// TODO: Combine with GameCube, and/or make a generic Nintendo region code class?
	// TODO: Specific European countries?
	const char *s_region;
	switch (region_char) {
		case '\0':
		case 'A':
			s_region = C_("WiiWAD|Region", "Region-Free");
			break;
		case 'E':
			s_region = C_("WiiWAD|Region", "USA");
			break;
		case 'J':
			s_region = C_("WiiWAD|Region", "Japan");
			break;
		case 'W':
			s_region = C_("WiiWAD|Region", "Taiwan");
			break;
		case 'K':
		case 'T':
		case 'Q':
			s_region = C_("WiiWAD|Region", "South Korea");
			break;
		case 'C':
			s_region = C_("WiiWAD|Region", "China");
			break;
		default:
			if (isupper(region_char)) {
				s_region = C_("WiiWAD|Region", "Europe");
			} else {
				s_region = nullptr;
			}
			break;
	}
	if (s_region) {
		d->fields->addField_string(C_("WiiWAD", "Region"), s_region);
	} else {
		d->fields->addField_string(C_("WiiWAD", "Region"),
			rp_sprintf(C_("WiiWAD", "Unknown (0x%02X)"), (uint8_t)region_char));
	}

	// Required IOS version.
	const uint32_t ios_lo = be32_to_cpu(tmdHeader->sys_version.lo);
	if (tmdHeader->sys_version.hi == cpu_to_be32(0x00000001) &&
	    ios_lo > 2 && ios_lo < 0x300)
	{
		// Standard IOS slot.
		d->fields->addField_string(C_("WiiWAD", "IOS Version"),
			rp_sprintf("IOS%u", ios_lo));
	} else if (tmdHeader->sys_version.id != 0) {
		// Non-standard IOS slot.
		// Print the full title ID.
		d->fields->addField_string(C_("WiiWAD", "IOS Version"),
			rp_sprintf("%08X-%08X", be32_to_cpu(tmdHeader->sys_version.hi), be32_to_cpu(tmdHeader->sys_version.lo)));
	}

	// Encryption key.
	// TODO: WiiPartition function to get a key's "display name"?
	static const char *const encKeyNames[] = {
		NOP_C_("WiiWAD|EncKey", "Retail"),
		NOP_C_("WiiWAD|EncKey", "Korean"),
		NOP_C_("WiiWAD|EncKey", "vWii"),
		NOP_C_("WiiWAD|EncKey", "SD AES"),
		NOP_C_("WiiWAD|EncKey", "SD IV"),
		NOP_C_("WiiWAD|EncKey", "SD MD5"),
		NOP_C_("WiiWAD|EncKey", "Debug"),
	};
	const char *keyName;
	if (d->key_idx >= 0 && d->key_idx < WiiPartition::Key_Max) {
		keyName = dpgettext_expr(RP_I18N_DOMAIN, "WiiWAD|EncKey", encKeyNames[d->key_idx]);
	} else {
		keyName = C_("WiiWAD", "Unknown");
	}
	d->fields->addField_string(C_("WiiWAD", "Encryption Key"), keyName);

	// Game info.
	string gameInfo = d->getGameInfo();
	if (!gameInfo.empty()) {
		d->fields->addField_string(C_("WiiWAD", "Game Info"), gameInfo);
	}

	// TODO: Decrypt content.bin to get the actual data.

	// Finished reading the field data.
	return (int)d->fields->count();
}

/**
 * Load metadata properties.
 * Called by RomData::metaData() if the field data hasn't been loaded yet.
 * @return Number of metadata properties read on success; negative POSIX error code on error.
 */
int WiiWAD::loadMetaData(void)
{
	RP_D(WiiWAD);
	if (d->metaData != nullptr) {
		// Metadata *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// NOTE: We can only get the title if the encryption key is valid.
	// If we can't get the title, don't bother creating RomMetaData.
	string gameInfo = d->getGameInfo();
	if (gameInfo.empty()) {
		return -EIO;
	}
	const size_t nl_pos = gameInfo.find('\n');
	if (nl_pos != string::npos) {
		gameInfo.resize(nl_pos);
	}
	if (gameInfo.empty()) {
		return -EIO;
	}

	// Create the metadata object.
	d->metaData = new RomMetaData();
	d->metaData->reserve(1);	// Maximum of 1 metadata property.

	// Title. (first line of game info)
	d->metaData->addMetaData_string(Property::Title, gameInfo);

	// Finished reading the metadata.
	return static_cast<int>(d->metaData->count());
}

/**
 * Get a list of URLs for an external image type.
 *
 * A thumbnail size may be requested from the shell.
 * If the subclass supports multiple sizes, it should
 * try to get the size that most closely matches the
 * requested size.
 *
 * @param imageType	[in]     Image type.
 * @param pExtURLs	[out]    Output vector.
 * @param size		[in,opt] Requested image size. This may be a requested
 *                               thumbnail size in pixels, or an ImageSizeType
 *                               enum value.
 * @return 0 on success; negative POSIX error code on error.
 */
int WiiWAD::extURLs(ImageType imageType, vector<ExtURL> *pExtURLs, int size) const
{
	assert(imageType >= IMG_EXT_MIN && imageType <= IMG_EXT_MAX);
	if (imageType < IMG_EXT_MIN || imageType > IMG_EXT_MAX) {
		// ImageType is out of range.
		return -ERANGE;
	}
	assert(pExtURLs != nullptr);
	if (!pExtURLs) {
		// No vector.
		return -EINVAL;
	}
	pExtURLs->clear();

	// Check for a valid TID hi.
	RP_D(const WiiWAD);
	switch (be32_to_cpu(d->tmdHeader.title_id.hi)) {
		case 0x00010000:
		case 0x00010001:
		case 0x00010002:
		case 0x00010004:
		case 0x00010005:
		case 0x00010008:
			// TID hi is valid.
			break;

		default:
			// No GameTDB artwork is available.
			return -ENOENT;
	}

	// Get the image sizes and sort them based on the
	// requested image size.
	vector<ImageSizeDef> sizeDefs = supportedImageSizes(imageType);
	if (sizeDefs.empty()) {
		// No image sizes.
		return -ENOENT;
	}

	// Select the best size.
	const ImageSizeDef *const sizeDef = d->selectBestSize(sizeDefs, size);
	if (!sizeDef) {
		// No size available...
		return -ENOENT;
	}

	// NOTE: Only downloading the first size as per the
	// sort order, since GameTDB basically guarantees that
	// all supported sizes for an image type are available.
	// TODO: Add cache keys for other sizes in case they're
	// downloaded and none of these are available?

	// Determine the image type name.
	const char *imageTypeName_base;
	const char *ext;
	switch (imageType) {
		case IMG_EXT_COVER:
			imageTypeName_base = "cover";
			ext = ".png";
			break;
		case IMG_EXT_COVER_3D:
			imageTypeName_base = "cover3D";
			ext = ".png";
			break;
		case IMG_EXT_COVER_FULL:
			imageTypeName_base = "coverfull";
			ext = ".png";
			break;
		case IMG_EXT_TITLE_SCREEN:
			imageTypeName_base = "wwtitle";
			ext = ".png";
			break;
		default:
			// Unsupported image type.
			return -ENOENT;
	}

	// Game ID. (GameTDB uses ID4 for WiiWare.)
	// The ID4 cannot have non-printable characters.
	// NOTE: Must be NULL-terminated.
	char id4[5];
	memcpy(id4, &d->tmdHeader.title_id.u8[4], 4);
	id4[4] = 0;
	for (int i = 4-1; i >= 0; i--) {
		if (!ISPRINT(id4[i])) {
			// Non-printable character found.
			return -ENOENT;
		}
	}

	// Determine the GameTDB region code(s).
	// TODO: Get the actual Wii region from the WAD file.
	vector<const char*> tdb_regions = d->wadRegionToGameTDB(id4[3]);

	// If we're downloading a "high-resolution" image (M or higher),
	// also add the default image to ExtURLs in case the user has
	// high-resolution image downloads disabled.
	const ImageSizeDef *szdefs_dl[2];
	szdefs_dl[0] = sizeDef;
	unsigned int szdef_count;
	if (sizeDef->index >= 2) {
		// M or higher.
		szdefs_dl[1] = &sizeDefs[0];
		szdef_count = 2;
	} else {
		// Default or S.
		szdef_count = 1;
	}

	// Add the URLs.
	pExtURLs->resize(szdef_count * tdb_regions.size());
	auto extURL_iter = pExtURLs->begin();
	for (unsigned int i = 0; i < szdef_count; i++) {
		// Current image type.
		char imageTypeName[16];
		snprintf(imageTypeName, sizeof(imageTypeName), "%s%s",
			 imageTypeName_base, (szdefs_dl[i]->name ? szdefs_dl[i]->name : ""));

		// Add the images.
		for (auto tdb_iter = tdb_regions.cbegin();
		     tdb_iter != tdb_regions.cend(); ++tdb_iter, ++extURL_iter)
		{
			extURL_iter->url = d->getURL_GameTDB("wii", imageTypeName, *tdb_iter, id4, ext);
			extURL_iter->cache_key = d->getCacheKey_GameTDB("wii", imageTypeName, *tdb_iter, id4, ext);
			extURL_iter->width = szdefs_dl[i]->width;
			extURL_iter->height = szdefs_dl[i]->height;
			extURL_iter->high_res = (szdefs_dl[i]->index >= 2);
		}
	}

	// All URLs added.
	return 0;
}

}
