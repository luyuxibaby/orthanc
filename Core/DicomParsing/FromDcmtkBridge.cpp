/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2019 Osimis S.A., Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "../PrecompiledHeaders.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#if !defined(ORTHANC_SANDBOXED)
#  error The macro ORTHANC_SANDBOXED must be defined
#endif

#if !defined(DCMTK_VERSION_NUMBER)
#  error The macro DCMTK_VERSION_NUMBER must be defined
#endif

#include "FromDcmtkBridge.h"
#include "ToDcmtkBridge.h"
#include "../Logging.h"
#include "../Toolbox.h"
#include "../OrthancException.h"

#if ORTHANC_SANDBOXED == 0
#  include "../TemporaryFile.h"
#endif

#include <list>
#include <limits>

#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>

#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcdicent.h>
#include <dcmtk/dcmdata/dcdict.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcostrmb.h>
#include <dcmtk/dcmdata/dcpixel.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcistrmb.h>

#include <dcmtk/dcmdata/dcvrae.h>
#include <dcmtk/dcmdata/dcvras.h>
#include <dcmtk/dcmdata/dcvrat.h>
#include <dcmtk/dcmdata/dcvrcs.h>
#include <dcmtk/dcmdata/dcvrda.h>
#include <dcmtk/dcmdata/dcvrds.h>
#include <dcmtk/dcmdata/dcvrdt.h>
#include <dcmtk/dcmdata/dcvrfd.h>
#include <dcmtk/dcmdata/dcvrfl.h>
#include <dcmtk/dcmdata/dcvris.h>
#include <dcmtk/dcmdata/dcvrlo.h>
#include <dcmtk/dcmdata/dcvrlt.h>
#include <dcmtk/dcmdata/dcvrpn.h>
#include <dcmtk/dcmdata/dcvrsh.h>
#include <dcmtk/dcmdata/dcvrsl.h>
#include <dcmtk/dcmdata/dcvrss.h>
#include <dcmtk/dcmdata/dcvrst.h>
#include <dcmtk/dcmdata/dcvrtm.h>
#include <dcmtk/dcmdata/dcvrui.h>
#include <dcmtk/dcmdata/dcvrul.h>
#include <dcmtk/dcmdata/dcvrus.h>
#include <dcmtk/dcmdata/dcvrut.h>

#if DCMTK_VERSION_NUMBER >= 361
#  include <dcmtk/dcmdata/dcvruc.h>
#  include <dcmtk/dcmdata/dcvrur.h>
#endif

#if DCMTK_USE_EMBEDDED_DICTIONARIES == 1
#  include <EmbeddedResources.h>
#endif

#if ORTHANC_ENABLE_DCMTK_JPEG == 1
#  include <dcmtk/dcmjpeg/djdecode.h>
#endif

#if ORTHANC_ENABLE_DCMTK_JPEG_LOSSLESS == 1
#  include <dcmtk/dcmjpls/djdecode.h>
#endif


namespace Orthanc
{
#if DCMTK_USE_EMBEDDED_DICTIONARIES == 1
  static void LoadEmbeddedDictionary(DcmDataDictionary& dictionary,
                                     EmbeddedResources::FileResourceId resource)
  {
    std::string content;
    EmbeddedResources::GetFileResource(content, resource);

#if ORTHANC_SANDBOXED == 0
    TemporaryFile tmp;
    tmp.Write(content);

    if (!dictionary.loadDictionary(tmp.GetPath().c_str()))
    {
      throw OrthancException(ErrorCode_InternalError,
                             "Cannot read embedded dictionary. Under Windows, make sure that " 
                             "your TEMP directory does not contain special characters.");
    }
#else
    if (!dictionary.loadFromMemory(content))
    {
      throw OrthancException(ErrorCode_InternalError,
                             "Cannot read embedded dictionary. Under Windows, make sure that " 
                             "your TEMP directory does not contain special characters.");
    }
#endif
  }
#endif


  namespace
  {
    class DictionaryLocker
    {
    private:
      DcmDataDictionary& dictionary_;

    public:
      DictionaryLocker() : dictionary_(dcmDataDict.wrlock())
      {
      }

      ~DictionaryLocker()
      {
#if DCMTK_VERSION_NUMBER >= 364
        dcmDataDict.wrunlock();
#else
        dcmDataDict.unlock();
#endif
      }

      DcmDataDictionary& operator*()
      {
        return dictionary_;
      }

      DcmDataDictionary* operator->()
      {
        return &dictionary_;
      }
    };

    
#define DCMTK_TO_CTYPE_CONVERTER(converter, cType, dcmtkType, getter) \
 \
    struct converter \
    { \
      typedef cType CType; \
 \
      static bool Apply(CType& result, \
                        DcmElement& element, \
                        size_t i) \
      { \
        return dynamic_cast<dcmtkType&>(element).getter(result, i).good(); \
      } \
    };

DCMTK_TO_CTYPE_CONVERTER(DcmtkToSint32Converter, Sint32, DcmSignedLong, getSint32)
DCMTK_TO_CTYPE_CONVERTER(DcmtkToSint16Converter, Sint16, DcmSignedShort, getSint16)
DCMTK_TO_CTYPE_CONVERTER(DcmtkToUint32Converter, Uint32, DcmUnsignedLong, getUint32)
DCMTK_TO_CTYPE_CONVERTER(DcmtkToUint16Converter, Uint16, DcmUnsignedShort, getUint16)
DCMTK_TO_CTYPE_CONVERTER(DcmtkToFloat32Converter, Float32, DcmFloatingPointSingle, getFloat32)
DCMTK_TO_CTYPE_CONVERTER(DcmtkToFloat64Converter, Float64, DcmFloatingPointDouble, getFloat64)


    template <typename F>
    static DicomValue* ApplyDcmtkToCTypeConverter(DcmElement& element)
    {
      F f;
      typename F::CType value;

      if (element.getLength() > sizeof(typename F::CType)
          && (element.getLength() % sizeof(typename F::CType)) == 0)
      {
        size_t count = element.getLength() / sizeof(typename F::CType);
        std::vector<std::string> strings;
        for (size_t i = 0; i < count; i++) {
          if (f.Apply(value, element, i)) {
            strings.push_back(boost::lexical_cast<std::string>(value));
          }
        }
        return new DicomValue(boost::algorithm::join(strings, "\\"), false);
      }
      else if (f.Apply(value, element, 0)) {
        return new DicomValue(boost::lexical_cast<std::string>(value), false);
      }
      else {
        return new DicomValue;
      }
    }

  }


  void FromDcmtkBridge::InitializeDictionary(bool loadPrivateDictionary)
  {
    LOG(INFO) << "Using DCTMK version: " << DCMTK_VERSION_NUMBER;
    
    {
      DictionaryLocker locker;

      locker->clear();

#if DCMTK_USE_EMBEDDED_DICTIONARIES == 1
      LOG(INFO) << "Loading the embedded dictionaries";
      /**
       * Do not load DICONDE dictionary, it breaks the other tags. The
       * command "strace storescu 2>&1 |grep dic" shows that DICONDE
       * dictionary is not loaded by storescu.
       **/
      //LoadEmbeddedDictionary(*locker, EmbeddedResources::DICTIONARY_DICONDE);

      LoadEmbeddedDictionary(*locker, EmbeddedResources::DICTIONARY_DICOM);

      if (loadPrivateDictionary)
      {
        LOG(INFO) << "Loading the embedded dictionary of private tags";
        LoadEmbeddedDictionary(*locker, EmbeddedResources::DICTIONARY_PRIVATE);
      }
      else
      {
        LOG(INFO) << "The dictionary of private tags has not been loaded";
      }

#else
      std::vector<std::string> dictionaries;
      
      const char* env = std::getenv(DCM_DICT_ENVIRONMENT_VARIABLE);
      if (env != NULL)
      {
        // This mimics the behavior of DCMTK:
        // https://support.dcmtk.org/docs/file_envvars.html
#if defined(_WIN32)
        Toolbox::TokenizeString(dictionaries, std::string(env), ';');
#else
        Toolbox::TokenizeString(dictionaries, std::string(env), ':');
#endif
      }
      else
      {
        boost::filesystem::path base = DCMTK_DICTIONARY_DIR;
        dictionaries.push_back((base / "dicom.dic").string());
        dictionaries.push_back((base / "private.dic").string());
      }

      for (size_t i = 0; i < dictionaries.size(); i++)
      {
        LOG(WARNING) << "Loading external DICOM dictionary: \"" << dictionaries[i] << "\"";
        
        if (!locker->loadDictionary(dictionaries[i].c_str()))
        {
          throw OrthancException(ErrorCode_InexistentFile);
        }
      }

#endif
    }

    /* make sure data dictionary is loaded */
    if (!dcmDataDict.isDictionaryLoaded())
    {
      throw OrthancException(ErrorCode_InternalError,
                             "No DICOM dictionary loaded, check environment variable: " +
                             std::string(DCM_DICT_ENVIRONMENT_VARIABLE));
    }

    {
      // Test the dictionary with a simple DICOM tag
      DcmTag key(0x0010, 0x1030); // This is PatientWeight
      if (key.getEVR() != EVR_DS)
      {
        throw OrthancException(ErrorCode_InternalError,
                               "The DICOM dictionary has not been correctly read");
      }
    }
  }


  void FromDcmtkBridge::RegisterDictionaryTag(const DicomTag& tag,
                                              ValueRepresentation vr,
                                              const std::string& name,
                                              unsigned int minMultiplicity,
                                              unsigned int maxMultiplicity,
                                              const std::string& privateCreator)
  {
    if (minMultiplicity < 1)
    {
      throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    bool arbitrary = false;
    if (maxMultiplicity == 0)
    {
      maxMultiplicity = DcmVariableVM;
      arbitrary = true;
    }
    else if (maxMultiplicity < minMultiplicity)
    {
      throw OrthancException(ErrorCode_ParameterOutOfRange);
    }
    
    DcmEVR evr = ToDcmtkBridge::Convert(vr);

    LOG(INFO) << "Registering tag in dictionary: " << tag << " " << (DcmVR(evr).getValidVRName()) << " " 
              << name << " (multiplicity: " << minMultiplicity << "-" 
              << (arbitrary ? "n" : boost::lexical_cast<std::string>(maxMultiplicity)) << ")";

    std::auto_ptr<DcmDictEntry>  entry;
    if (privateCreator.empty())
    {
      if (tag.GetGroup() % 2 == 1)
      {
        char buf[128];
        sprintf(buf, "Warning: You are registering a private tag (%04x,%04x), "
                "but no private creator was associated with it", 
                tag.GetGroup(), tag.GetElement());
        LOG(WARNING) << buf;
      }

      entry.reset(new DcmDictEntry(tag.GetGroup(),
                                   tag.GetElement(),
                                   evr, name.c_str(),
                                   static_cast<int>(minMultiplicity),
                                   static_cast<int>(maxMultiplicity),
                                   NULL    /* version */,
                                   OFTrue  /* doCopyString */,
                                   NULL    /* private creator */));
    }
    else
    {
      // "Private Data Elements have an odd Group Number that is not
      // (0001,eeee), (0003,eeee), (0005,eeee), (0007,eeee), or
      // (FFFF,eeee)."
      if (tag.GetGroup() % 2 == 0 /* even */ ||
          tag.GetGroup() == 0x0001 ||
          tag.GetGroup() == 0x0003 ||
          tag.GetGroup() == 0x0005 ||
          tag.GetGroup() == 0x0007 ||
          tag.GetGroup() == 0xffff)
      {
        char buf[128];
        sprintf(buf, "Trying to register private tag (%04x,%04x), but it must have an odd group >= 0x0009",
                tag.GetGroup(), tag.GetElement());
        throw OrthancException(ErrorCode_ParameterOutOfRange, std::string(buf));
      }

      entry.reset(new DcmDictEntry(tag.GetGroup(),
                                   tag.GetElement(),
                                   evr, name.c_str(),
                                   static_cast<int>(minMultiplicity),
                                   static_cast<int>(maxMultiplicity),
                                   "private" /* version */,
                                   OFTrue    /* doCopyString */,
                                   privateCreator.c_str()));
    }

    entry->setGroupRangeRestriction(DcmDictRange_Unspecified);
    entry->setElementRangeRestriction(DcmDictRange_Unspecified);

    {
      DictionaryLocker locker;

      if (locker->findEntry(name.c_str()))
      {
        throw OrthancException(ErrorCode_AlreadyExistingTag,
                               "Cannot register two tags with the same symbolic name \"" + name + "\"");
      }

      locker->addEntry(entry.release());
    }
  }


  Encoding FromDcmtkBridge::DetectEncoding(bool& hasCodeExtensions,
                                           DcmItem& dataset,
                                           Encoding defaultEncoding)
  {
    // http://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html#sect_C.12.1.1.2

    OFString tmp;
    if (dataset.findAndGetOFStringArray(DCM_SpecificCharacterSet, tmp).good())
    {
      std::vector<std::string> tokens;
      Toolbox::TokenizeString(tokens, std::string(tmp.c_str()), '\\');

      hasCodeExtensions = (tokens.size() > 1);

      for (size_t i = 0; i < tokens.size(); i++)
      {
        std::string characterSet = Toolbox::StripSpaces(tokens[i]);

        if (!characterSet.empty())
        {
          Encoding encoding;
          
          if (GetDicomEncoding(encoding, characterSet.c_str()))
          {
            // The specific character set is supported by the Orthanc core
            return encoding;
          }
          else
          {
            LOG(WARNING) << "Value of Specific Character Set (0008,0005) is not supported: " << characterSet
                         << ", fallback to ASCII (remove all special characters)";
            return Encoding_Ascii;
          }
        }
      }
    }
    else
    {
      hasCodeExtensions = false;
    }
    
    // No specific character set tag: Use the default encoding
    return defaultEncoding;
  }


  void FromDcmtkBridge::ExtractDicomSummary(DicomMap& target, 
                                            DcmItem& dataset,
                                            unsigned int maxStringLength,
                                            Encoding defaultEncoding)
  {
    std::set<DicomTag> ignoreTagLength;

    bool hasCodeExtensions;
    Encoding encoding = DetectEncoding(hasCodeExtensions, dataset, defaultEncoding);

    target.Clear();
    for (unsigned long i = 0; i < dataset.card(); i++)
    {
      DcmElement* element = dataset.getElement(i);
      if (element && element->isLeaf())
      {
        target.SetValue(element->getTag().getGTag(),
                        element->getTag().getETag(),
                        ConvertLeafElement(*element, DicomToJsonFlags_Default,
                                           maxStringLength, encoding, hasCodeExtensions, ignoreTagLength));
      }
    }
  }


  DicomTag FromDcmtkBridge::Convert(const DcmTag& tag)
  {
    return DicomTag(tag.getGTag(), tag.getETag());
  }


  DicomTag FromDcmtkBridge::GetTag(const DcmElement& element)
  {
    return DicomTag(element.getGTag(), element.getETag());
  }


  DicomValue* FromDcmtkBridge::ConvertLeafElement(DcmElement& element,
                                                  DicomToJsonFlags flags,
                                                  unsigned int maxStringLength,
                                                  Encoding encoding,
                                                  bool hasCodeExtensions,
                                                  const std::set<DicomTag>& ignoreTagLength)
  {
    if (!element.isLeaf())
    {
      // This function is only applicable to leaf elements
      throw OrthancException(ErrorCode_BadParameterType);
    }

    char *c = NULL;
    if (element.isaString() &&
        element.getString(c).good())
    {
      if (c == NULL)  // This case corresponds to the empty string
      {
        return new DicomValue("", false);
      }
      else
      {
        std::string s(c);
        std::string utf8 = Toolbox::ConvertToUtf8(s, encoding, hasCodeExtensions);

        if (maxStringLength != 0 &&
            utf8.size() > maxStringLength &&
            ignoreTagLength.find(GetTag(element)) == ignoreTagLength.end())
        {
          return new DicomValue;  // Too long, create a NULL value
        }
        else
        {
          return new DicomValue(utf8, false);
        }
      }
    }


    if (element.getVR() == EVR_UN)
    {
      // Unknown value representation: Lookup in the dictionary. This
      // is notably the case for private tags registered with the
      // "Dictionary" configuration option.
      DictionaryLocker locker;
      
      const DcmDictEntry* entry = locker->findEntry(element.getTag().getXTag(), 
                                                    element.getTag().getPrivateCreator());
      if (entry != NULL && 
          entry->getVR().isaString())
      {
        Uint8* data = NULL;

        // At (*), we do not try and convert to UTF-8, as nothing says
        // the encoding of the private tag is the same as that of the
        // remaining of the DICOM dataset. Only go for ASCII strings.

        if (element.getUint8Array(data) == EC_Normal &&
            Toolbox::IsAsciiString(data, element.getLength()))   // (*)
        {
          if (data == NULL)
          {
            return new DicomValue("", false);   // Empty string
          }
          else if (maxStringLength != 0 &&
                   element.getLength() > maxStringLength &&
                   ignoreTagLength.find(GetTag(element)) == ignoreTagLength.end())
          {
            return new DicomValue;  // Too long, create a NULL value
          }
          else
          {
            std::string s(reinterpret_cast<const char*>(data), element.getLength());
            return new DicomValue(s, false);
          }
        }
      }
    }

    
    try
    {
      // http://support.dcmtk.org/docs/dcvr_8h-source.html
      switch (element.getVR())
      {

        /**
         * Deal with binary data (including PixelData).
         **/

        case EVR_OB:  // other byte
        case EVR_OF:  // other float
        case EVR_OW:  // other word
        case EVR_UN:  // unknown value representation
        case EVR_ox:  // OB or OW depending on context
        case EVR_DS:  // decimal string
        case EVR_IS:  // integer string
        case EVR_AS:  // age string
        case EVR_DA:  // date string
        case EVR_DT:  // date time string
        case EVR_TM:  // time string
        case EVR_AE:  // application entity title
        case EVR_CS:  // code string
        case EVR_SH:  // short string
        case EVR_LO:  // long string
        case EVR_ST:  // short text
        case EVR_LT:  // long text
        case EVR_UT:  // unlimited text
        case EVR_PN:  // person name
        case EVR_UI:  // unique identifier
        case EVR_UNKNOWN: // used internally for elements with unknown VR (encoded with 4-byte length field in explicit VR)
        case EVR_UNKNOWN2B:  // used internally for elements with unknown VR with 2-byte length field in explicit VR
        {
          if (!(flags & DicomToJsonFlags_ConvertBinaryToNull))
          {
            Uint8* data = NULL;
            if (element.getUint8Array(data) == EC_Normal)
            {
              return new DicomValue(reinterpret_cast<const char*>(data), element.getLength(), true);
            }
          }

          return new DicomValue;
        }
    
        /**
         * Numeric types
         **/ 
      
        case EVR_SL:  // signed long
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToSint32Converter>(element);
        }

        case EVR_SS:  // signed short
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToSint16Converter>(element);
        }

        case EVR_UL:  // unsigned long
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToUint32Converter>(element);
        }

        case EVR_US:  // unsigned short
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToUint16Converter>(element);
        }

        case EVR_FL:  // float single-precision
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToFloat32Converter>(element);
        }

        case EVR_FD:  // float double-precision
        {
          return ApplyDcmtkToCTypeConverter<DcmtkToFloat64Converter>(element);
        }


        /**
         * Attribute tag.
         **/

        case EVR_AT:
        {
          DcmTagKey tag;
          if (dynamic_cast<DcmAttributeTag&>(element).getTagVal(tag, 0).good())
          {
            DicomTag t(tag.getGroup(), tag.getElement());
            return new DicomValue(t.Format(), false);
          }
          else
          {
            return new DicomValue;
          }
        }


        /**
         * Sequence types, should never occur at this point because of
         * "element.isLeaf()".
         **/

        case EVR_SQ:  // sequence of items
          return new DicomValue;


          /**
           * Internal to DCMTK.
           **/ 

        case EVR_xs:  // SS or US depending on context
        case EVR_lt:  // US, SS or OW depending on context, used for LUT Data (thus the name)
        case EVR_na:  // na="not applicable", for data which has no VR
        case EVR_up:  // up="unsigned pointer", used internally for DICOMDIR suppor
        case EVR_item:  // used internally for items
        case EVR_metainfo:  // used internally for meta info datasets
        case EVR_dataset:  // used internally for datasets
        case EVR_fileFormat:  // used internally for DICOM files
        case EVR_dicomDir:  // used internally for DICOMDIR objects
        case EVR_dirRecord:  // used internally for DICOMDIR records
        case EVR_pixelSQ:  // used internally for pixel sequences in a compressed image
        case EVR_pixelItem:  // used internally for pixel items in a compressed image
        case EVR_PixelData:  // used internally for uncompressed pixeld data
        case EVR_OverlayData:  // used internally for overlay data
          return new DicomValue;


          /**
           * Default case.
           **/ 

        default:
          return new DicomValue;
      }
    }
    catch (boost::bad_lexical_cast&)
    {
      return new DicomValue;
    }
    catch (std::bad_cast&)
    {
      return new DicomValue;
    }
  }


  static Json::Value& PrepareNode(Json::Value& parent,
                                  DcmElement& element,
                                  DicomToJsonFormat format)
  {
    assert(parent.type() == Json::objectValue);

    DicomTag tag(FromDcmtkBridge::GetTag(element));
    const std::string formattedTag = tag.Format();

    if (format == DicomToJsonFormat_Short)
    {
      parent[formattedTag] = Json::nullValue;
      return parent[formattedTag];
    }

    // This code gives access to the name of the private tags
    std::string tagName = FromDcmtkBridge::GetTagName(element);
    
    switch (format)
    {
      case DicomToJsonFormat_Human:
        parent[tagName] = Json::nullValue;
        return parent[tagName];

      case DicomToJsonFormat_Full:
      {
        parent[formattedTag] = Json::objectValue;
        Json::Value& node = parent[formattedTag];

        if (element.isLeaf())
        {
          node["Name"] = tagName;

          if (element.getTag().getPrivateCreator() != NULL)
          {
            node["PrivateCreator"] = element.getTag().getPrivateCreator();
          }

          return node;
        }
        else
        {
          node["Name"] = tagName;
          node["Type"] = "Sequence";
          node["Value"] = Json::nullValue;
          return node["Value"];
        }
      }

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }
  }


  static void LeafValueToJson(Json::Value& target,
                              const DicomValue& value,
                              DicomToJsonFormat format,
                              DicomToJsonFlags flags,
                              unsigned int maxStringLength)
  {
    Json::Value* targetValue = NULL;
    Json::Value* targetType = NULL;

    switch (format)
    {
      case DicomToJsonFormat_Short:
      case DicomToJsonFormat_Human:
      {
        assert(target.type() == Json::nullValue);
        targetValue = &target;
        break;
      }      

      case DicomToJsonFormat_Full:
      {
        assert(target.type() == Json::objectValue);
        target["Value"] = Json::nullValue;
        target["Type"] = Json::nullValue;
        targetType = &target["Type"];
        targetValue = &target["Value"];
        break;
      }

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }

    assert(targetValue != NULL);
    assert(targetValue->type() == Json::nullValue);
    assert(targetType == NULL || targetType->type() == Json::nullValue);

    if (value.IsNull())
    {
      if (targetType != NULL)
      {
        *targetType = "Null";
      }
    }
    else if (value.IsBinary())
    {
      if (flags & DicomToJsonFlags_ConvertBinaryToAscii)
      {
        *targetValue = Toolbox::ConvertToAscii(value.GetContent());
      }
      else
      {
        std::string s;
        value.FormatDataUriScheme(s);
        *targetValue = s;
      }

      if (targetType != NULL)
      {
        *targetType = "Binary";
      }
    }
    else if (maxStringLength == 0 ||
             value.GetContent().size() <= maxStringLength)
    {
      *targetValue = value.GetContent();

      if (targetType != NULL)
      {
        *targetType = "String";
      }
    }
    else
    {
      if (targetType != NULL)
      {
        *targetType = "TooLong";
      }
    }
  }                              


  void FromDcmtkBridge::ElementToJson(Json::Value& parent,
                                      DcmElement& element,
                                      DicomToJsonFormat format,
                                      DicomToJsonFlags flags,
                                      unsigned int maxStringLength,
                                      Encoding encoding,
                                      bool hasCodeExtensions,
                                      const std::set<DicomTag>& ignoreTagLength)
  {
    if (parent.type() == Json::nullValue)
    {
      parent = Json::objectValue;
    }

    assert(parent.type() == Json::objectValue);
    Json::Value& target = PrepareNode(parent, element, format);

    if (element.isLeaf())
    {
      // The "0" below lets "LeafValueToJson()" take care of "TooLong" values
      std::auto_ptr<DicomValue> v(FromDcmtkBridge::ConvertLeafElement
                                  (element, flags, 0, encoding, hasCodeExtensions, ignoreTagLength));

      if (ignoreTagLength.find(GetTag(element)) == ignoreTagLength.end())
      {
        LeafValueToJson(target, *v, format, flags, maxStringLength);
      }
      else
      {
        LeafValueToJson(target, *v, format, flags, 0);
      }
    }
    else
    {
      assert(target.type() == Json::nullValue);
      target = Json::arrayValue;

      // "All subclasses of DcmElement except for DcmSequenceOfItems
      // are leaf nodes, while DcmSequenceOfItems, DcmItem, DcmDataset
      // etc. are not." The following dynamic_cast is thus OK.
      DcmSequenceOfItems& sequence = dynamic_cast<DcmSequenceOfItems&>(element);

      for (unsigned long i = 0; i < sequence.card(); i++)
      {
        DcmItem* child = sequence.getItem(i);
        Json::Value& v = target.append(Json::objectValue);
        DatasetToJson(v, *child, format, flags, maxStringLength, encoding, hasCodeExtensions, ignoreTagLength);
      }
    }
  }


  void FromDcmtkBridge::DatasetToJson(Json::Value& parent,
                                      DcmItem& item,
                                      DicomToJsonFormat format,
                                      DicomToJsonFlags flags,
                                      unsigned int maxStringLength,
                                      Encoding encoding,
                                      bool hasCodeExtensions,
                                      const std::set<DicomTag>& ignoreTagLength)
  {
    assert(parent.type() == Json::objectValue);

    for (unsigned long i = 0; i < item.card(); i++)
    {
      DcmElement* element = item.getElement(i);
      if (element == NULL)
      {
        throw OrthancException(ErrorCode_InternalError);
      }

      DicomTag tag(FromDcmtkBridge::Convert(element->getTag()));

      /*element->getTag().isPrivate()*/
      if (tag.IsPrivate() &&
          !(flags & DicomToJsonFlags_IncludePrivateTags))    
      {
        continue;
      }

      if (!(flags & DicomToJsonFlags_IncludeUnknownTags))
      {
        DictionaryLocker locker;
        if (locker->findEntry(element->getTag(), NULL) == NULL)
        {
          continue;
        }
      }

      DcmEVR evr = element->getTag().getEVR();
      if (evr == EVR_OB ||
          evr == EVR_OF ||
          evr == EVR_OW ||
          evr == EVR_UN ||
          evr == EVR_ox)
      {
        // This is a binary tag
        if ((tag == DICOM_TAG_PIXEL_DATA && !(flags & DicomToJsonFlags_IncludePixelData)) ||
            (tag != DICOM_TAG_PIXEL_DATA && !(flags & DicomToJsonFlags_IncludeBinary)))
        {
          continue;
        }
      }

      FromDcmtkBridge::ElementToJson(parent, *element, format, flags,
                                     maxStringLength, encoding, hasCodeExtensions, ignoreTagLength);
    }
  }


  void FromDcmtkBridge::ExtractDicomAsJson(Json::Value& target, 
                                           DcmDataset& dataset,
                                           DicomToJsonFormat format,
                                           DicomToJsonFlags flags,
                                           unsigned int maxStringLength,
                                           Encoding defaultEncoding,
                                           const std::set<DicomTag>& ignoreTagLength)
  {
    bool hasCodeExtensions;
    Encoding encoding = DetectEncoding(hasCodeExtensions, dataset, defaultEncoding);

    target = Json::objectValue;
    DatasetToJson(target, dataset, format, flags, maxStringLength, encoding, hasCodeExtensions, ignoreTagLength);
  }


  void FromDcmtkBridge::ExtractHeaderAsJson(Json::Value& target, 
                                            DcmMetaInfo& dataset,
                                            DicomToJsonFormat format,
                                            DicomToJsonFlags flags,
                                            unsigned int maxStringLength)
  {
    std::set<DicomTag> ignoreTagLength;
    target = Json::objectValue;
    DatasetToJson(target, dataset, format, flags, maxStringLength, Encoding_Ascii, false, ignoreTagLength);
  }



  static std::string GetTagNameInternal(DcmTag& tag)
  {
    {
      // Some patches for important tags because of different DICOM
      // dictionaries between DCMTK versions
      DicomTag tmp(tag.getGroup(), tag.getElement());
      std::string n = tmp.GetMainTagsName();
      if (n.size() != 0)
      {
        return n;
      }
      // End of patches
    }

#if 0
    // This version explicitly calls the dictionary
    const DcmDataDictionary& dict = dcmDataDict.rdlock();
    const DcmDictEntry* entry = dict.findEntry(tag, NULL);

    std::string s(DcmTag_ERROR_TagName);
    if (entry != NULL)
    {
      s = std::string(entry->getTagName());
    }

    dcmDataDict.unlock();
    return s;
#else
    const char* name = tag.getTagName();
    if (name == NULL)
    {
      return DcmTag_ERROR_TagName;
    }
    else
    {
      return std::string(name);
    }
#endif
  }


  std::string FromDcmtkBridge::GetTagName(const DicomTag& t,
                                          const std::string& privateCreator)
  {
    DcmTag tag(t.GetGroup(), t.GetElement());

    if (!privateCreator.empty())
    {
      tag.setPrivateCreator(privateCreator.c_str());
    }

    return GetTagNameInternal(tag);
  }


  std::string FromDcmtkBridge::GetTagName(const DcmElement& element)
  {
    // Copy the tag to ensure const-correctness of DcmElement. Note
    // that the private creator information is also copied.
    DcmTag tag(element.getTag());  

    return GetTagNameInternal(tag);
  }



  DicomTag FromDcmtkBridge::ParseTag(const char* name)
  {
    DicomTag parsed(0, 0);
    if (DicomTag::ParseHexadecimal(parsed, name))
    {
      return parsed;
    }

#if 0
    const DcmDataDictionary& dict = dcmDataDict.rdlock();
    const DcmDictEntry* entry = dict.findEntry(name);

    if (entry == NULL)
    {
      dcmDataDict.unlock();
      throw OrthancException(ErrorCode_UnknownDicomTag);
    }
    else
    {
      DcmTagKey key = entry->getKey();
      DicomTag tag(key.getGroup(), key.getElement());
      dcmDataDict.unlock();
      return tag;
    }
#else
    DcmTag tag;
    if (DcmTag::findTagFromName(name, tag).good())
    {
      return DicomTag(tag.getGTag(), tag.getETag());
    }
    else
    {
      LOG(INFO) << "Unknown DICOM tag: \"" << name << "\"";
      throw OrthancException(ErrorCode_UnknownDicomTag);
    }
#endif
  }


  bool FromDcmtkBridge::IsUnknownTag(const DicomTag& tag)
  {
    DcmTag tmp(tag.GetGroup(), tag.GetElement());
    return tmp.isUnknownVR();
  }


  void FromDcmtkBridge::ToJson(Json::Value& result,
                               const DicomMap& values,
                               bool simplify)
  {
    if (result.type() != Json::objectValue)
    {
      throw OrthancException(ErrorCode_BadParameterType);
    }

    result.clear();

    for (DicomMap::Map::const_iterator 
           it = values.map_.begin(); it != values.map_.end(); ++it)
    {
      // TODO Inject PrivateCreator if some is available in the DicomMap?
      const std::string tagName = GetTagName(it->first, "");

      if (simplify)
      {
        if (it->second->IsNull())
        {
          result[tagName] = Json::nullValue;
        }
        else
        {
          // TODO IsBinary
          result[tagName] = it->second->GetContent();
        }
      }
      else
      {
        Json::Value value = Json::objectValue;

        value["Name"] = tagName;

        if (it->second->IsNull())
        {
          value["Type"] = "Null";
          value["Value"] = Json::nullValue;
        }
        else
        {
          // TODO IsBinary
          value["Type"] = "String";
          value["Value"] = it->second->GetContent();
        }

        result[it->first.Format()] = value;
      }
    }
  }


  std::string FromDcmtkBridge::GenerateUniqueIdentifier(ResourceType level)
  {
    char uid[100];

    switch (level)
    {
      case ResourceType_Patient:
        // The "PatientID" field is of type LO (Long String), 64
        // Bytes Maximum. An UUID is of length 36, thus it can be used
        // as a random PatientID.
        return Toolbox::GenerateUuid();

      case ResourceType_Instance:
        return dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT);

      case ResourceType_Series:
        return dcmGenerateUniqueIdentifier(uid, SITE_SERIES_UID_ROOT);

      case ResourceType_Study:
        return dcmGenerateUniqueIdentifier(uid, SITE_STUDY_UID_ROOT);

      default:
        throw OrthancException(ErrorCode_ParameterOutOfRange);
    }
  }

  bool FromDcmtkBridge::SaveToMemoryBuffer(std::string& buffer,
                                           DcmDataset& dataSet)
  {
    // Determine the transfer syntax which shall be used to write the
    // information to the file. We always switch to the Little Endian
    // syntax, with explicit length.

    // http://support.dcmtk.org/docs/dcxfer_8h-source.html


    /**
     * Note that up to Orthanc 0.7.1 (inclusive), the
     * "EXS_LittleEndianExplicit" was always used to save the DICOM
     * dataset into memory. We now keep the original transfer syntax
     * (if available).
     **/
    E_TransferSyntax xfer = dataSet.getOriginalXfer();
    if (xfer == EXS_Unknown)
    {
      // No information about the original transfer syntax: This is
      // most probably a DICOM dataset that was read from memory.
      xfer = EXS_LittleEndianExplicit;
    }

    E_EncodingType encodingType = /*opt_sequenceType*/ EET_ExplicitLength;

    // Create the meta-header information
    DcmFileFormat ff(&dataSet);
    ff.validateMetaInfo(xfer);
    ff.removeInvalidGroups();

    // Create a memory buffer with the proper size
    {
      const uint32_t estimatedSize = ff.calcElementLength(xfer, encodingType);  // (*)
      buffer.resize(estimatedSize);
    }

    DcmOutputBufferStream ob(&buffer[0], buffer.size());

    // Fill the memory buffer with the meta-header and the dataset
    ff.transferInit();
    OFCondition c = ff.write(ob, xfer, encodingType, NULL,
                             /*opt_groupLength*/ EGL_recalcGL,
                             /*opt_paddingType*/ EPD_withoutPadding);
    ff.transferEnd();

    if (c.good())
    {
      // The DICOM file is successfully written, truncate the target
      // buffer if its size was overestimated by (*)
      ob.flush();

      size_t effectiveSize = static_cast<size_t>(ob.tell());
      if (effectiveSize < buffer.size())
      {
        buffer.resize(effectiveSize);
      }

      return true;
    }
    else
    {
      // Error
      buffer.clear();
      return false;
    }
  }


  ValueRepresentation FromDcmtkBridge::LookupValueRepresentation(const DicomTag& tag)
  {
    DcmTag t(tag.GetGroup(), tag.GetElement());
    return Convert(t.getEVR());
  }

  ValueRepresentation FromDcmtkBridge::Convert(const DcmEVR vr)
  {
    switch (vr)
    {
      case EVR_AE:
        return ValueRepresentation_ApplicationEntity;

      case EVR_AS:
        return ValueRepresentation_AgeString;

      case EVR_AT:
        return ValueRepresentation_AttributeTag;

      case EVR_CS:
        return ValueRepresentation_CodeString;

      case EVR_DA:
        return ValueRepresentation_Date;

      case EVR_DS:
        return ValueRepresentation_DecimalString;

      case EVR_DT:
        return ValueRepresentation_DateTime;

      case EVR_FL:
        return ValueRepresentation_FloatingPointSingle;

      case EVR_FD:
        return ValueRepresentation_FloatingPointDouble;

      case EVR_IS:
        return ValueRepresentation_IntegerString;

      case EVR_LO:
        return ValueRepresentation_LongString;

      case EVR_LT:
        return ValueRepresentation_LongText;

      case EVR_OB:
        return ValueRepresentation_OtherByte;

#if DCMTK_VERSION_NUMBER >= 361
        case EVR_OD:
          return ValueRepresentation_OtherDouble;
#endif

      case EVR_OF:
        return ValueRepresentation_OtherFloat;

#if DCMTK_VERSION_NUMBER >= 362
        case EVR_OL:
          return ValueRepresentation_OtherLong;
#endif

      case EVR_OW:
        return ValueRepresentation_OtherWord;

      case EVR_PN:
        return ValueRepresentation_PersonName;

      case EVR_SH:
        return ValueRepresentation_ShortString;

      case EVR_SL:
        return ValueRepresentation_SignedLong;

      case EVR_SQ:
        return ValueRepresentation_Sequence;

      case EVR_SS:
        return ValueRepresentation_SignedShort;

      case EVR_ST:
        return ValueRepresentation_ShortText;

      case EVR_TM:
        return ValueRepresentation_Time;

#if DCMTK_VERSION_NUMBER >= 361
      case EVR_UC:
        return ValueRepresentation_UnlimitedCharacters;
#endif

      case EVR_UI:
        return ValueRepresentation_UniqueIdentifier;

      case EVR_UL:
        return ValueRepresentation_UnsignedLong;

      case EVR_UN:
        return ValueRepresentation_Unknown;

#if DCMTK_VERSION_NUMBER >= 361
      case EVR_UR:
        return ValueRepresentation_UniversalResource;
#endif

      case EVR_US:
        return ValueRepresentation_UnsignedShort;

      case EVR_UT:
        return ValueRepresentation_UnlimitedText;

      default:
        return ValueRepresentation_NotSupported;
    }
  }


  static bool IsBinaryTag(const DcmTag& key)
  {
    return (key.isUnknownVR() ||
#if DCMTK_VERSION_NUMBER >= 361
            key.getEVR() == EVR_OD ||
#endif
            
#if DCMTK_VERSION_NUMBER >= 362
            key.getEVR() == EVR_OL ||
#endif            
            key.getEVR() == EVR_OB ||
            key.getEVR() == EVR_OF ||
            key.getEVR() == EVR_OW ||
            key.getEVR() == EVR_UN ||
            key.getEVR() == EVR_ox);
  }


  DcmElement* FromDcmtkBridge::CreateElementForTag(const DicomTag& tag)
  {
    DcmTag key(tag.GetGroup(), tag.GetElement());

    if (tag.IsPrivate() ||
        IsBinaryTag(key))
    {
      return new DcmOtherByteOtherWord(key);
    }

    switch (key.getEVR())
    {
      // http://support.dcmtk.org/docs/dcvr_8h-source.html

      /**
       * Binary types, handled above
       **/
    
#if DCMTK_VERSION_NUMBER >= 361
      case EVR_OD:
#endif            

#if DCMTK_VERSION_NUMBER >= 362
      case EVR_OL:
#endif            

      case EVR_OB:  // other byte
      case EVR_OF:  // other float
      case EVR_OW:  // other word
      case EVR_UN:  // unknown value representation
      case EVR_ox:  // OB or OW depending on context
        throw OrthancException(ErrorCode_InternalError);


      /**
       * String types.
       * http://support.dcmtk.org/docs/classDcmByteString.html
       **/
      
      case EVR_AS:  // age string
        return new DcmAgeString(key);

      case EVR_AE:  // application entity title
        return new DcmApplicationEntity(key);

      case EVR_CS:  // code string
        return new DcmCodeString(key);        

      case EVR_DA:  // date string
        return new DcmDate(key);
        
      case EVR_DT:  // date time string
        return new DcmDateTime(key);

      case EVR_DS:  // decimal string
        return new DcmDecimalString(key);

      case EVR_IS:  // integer string
        return new DcmIntegerString(key);

      case EVR_TM:  // time string
        return new DcmTime(key);

      case EVR_UI:  // unique identifier
        return new DcmUniqueIdentifier(key);

      case EVR_ST:  // short text
        return new DcmShortText(key);

      case EVR_LO:  // long string
        return new DcmLongString(key);

      case EVR_LT:  // long text
        return new DcmLongText(key);

      case EVR_UT:  // unlimited text
        return new DcmUnlimitedText(key);

      case EVR_SH:  // short string
        return new DcmShortString(key);

      case EVR_PN:  // person name
        return new DcmPersonName(key);

#if DCMTK_VERSION_NUMBER >= 361
      case EVR_UC:  // unlimited characters
        return new DcmUnlimitedCharacters(key);
#endif

#if DCMTK_VERSION_NUMBER >= 361
      case EVR_UR:  // URI/URL
        return new DcmUniversalResourceIdentifierOrLocator(key);
#endif
          
        
      /**
       * Numerical types
       **/ 
      
      case EVR_SL:  // signed long
        return new DcmSignedLong(key);

      case EVR_SS:  // signed short
        return new DcmSignedShort(key);

      case EVR_UL:  // unsigned long
        return new DcmUnsignedLong(key);

      case EVR_US:  // unsigned short
        return new DcmUnsignedShort(key);

      case EVR_FL:  // float single-precision
        return new DcmFloatingPointSingle(key);

      case EVR_FD:  // float double-precision
        return new DcmFloatingPointDouble(key);


      /**
       * Sequence types, should never occur at this point.
       **/

      case EVR_SQ:  // sequence of items
        throw OrthancException(ErrorCode_ParameterOutOfRange);


      /**
       * TODO
       **/

      case EVR_AT:  // attribute tag
        throw OrthancException(ErrorCode_NotImplemented);


      /**
       * Internal to DCMTK.
       **/ 

      case EVR_xs:  // SS or US depending on context
      case EVR_lt:  // US, SS or OW depending on context, used for LUT Data (thus the name)
      case EVR_na:  // na="not applicable", for data which has no VR
      case EVR_up:  // up="unsigned pointer", used internally for DICOMDIR suppor
      case EVR_item:  // used internally for items
      case EVR_metainfo:  // used internally for meta info datasets
      case EVR_dataset:  // used internally for datasets
      case EVR_fileFormat:  // used internally for DICOM files
      case EVR_dicomDir:  // used internally for DICOMDIR objects
      case EVR_dirRecord:  // used internally for DICOMDIR records
      case EVR_pixelSQ:  // used internally for pixel sequences in a compressed image
      case EVR_pixelItem:  // used internally for pixel items in a compressed image
      case EVR_UNKNOWN: // used internally for elements with unknown VR (encoded with 4-byte length field in explicit VR)
      case EVR_PixelData:  // used internally for uncompressed pixeld data
      case EVR_OverlayData:  // used internally for overlay data
      case EVR_UNKNOWN2B:  // used internally for elements with unknown VR with 2-byte length field in explicit VR
      default:
        break;
    }

    throw OrthancException(ErrorCode_InternalError);          
  }



  void FromDcmtkBridge::FillElementWithString(DcmElement& element,
                                              const DicomTag& tag,
                                              const std::string& utf8Value,
                                              bool decodeDataUriScheme,
                                              Encoding dicomEncoding)
  {
    std::string binary;
    const std::string* decoded = &utf8Value;

    if (decodeDataUriScheme &&
        boost::starts_with(utf8Value, URI_SCHEME_PREFIX_BINARY))
    {
      std::string mime;
      if (!Toolbox::DecodeDataUriScheme(mime, binary, utf8Value))
      {
        throw OrthancException(ErrorCode_BadFileFormat);
      }

      decoded = &binary;
    }
    else if (dicomEncoding != Encoding_Utf8)
    {
      binary = Toolbox::ConvertFromUtf8(utf8Value, dicomEncoding);
      decoded = &binary;
    }

    DcmTag key(tag.GetGroup(), tag.GetElement());

    if (tag.IsPrivate() ||
        IsBinaryTag(key))
    {
      bool ok;

      switch (key.getEVR())
      {
        case EVR_OW:
          if (decoded->size() % sizeof(Uint16) != 0)
          {
            LOG(ERROR) << "A tag with OW VR must have an even number of bytes";
            ok = false;
          }
          else
          {
            ok = element.putUint16Array((const Uint16*) decoded->c_str(), decoded->size() / sizeof(Uint16)).good();
          }
          
          break;
      
        default:
          ok = element.putUint8Array((const Uint8*) decoded->c_str(), decoded->size()).good();
          break;
      }
      
      if (ok)
      {
        return;
      }
      else
      {
        throw OrthancException(ErrorCode_InternalError);
      }
    }

    bool ok = false;
    
    try
    {
      switch (key.getEVR())
      {
        // http://support.dcmtk.org/docs/dcvr_8h-source.html

        /**
         * TODO.
         **/

        case EVR_OB:  // other byte
        case EVR_OF:  // other float
        case EVR_OW:  // other word
        case EVR_AT:  // attribute tag
          throw OrthancException(ErrorCode_NotImplemented);
    
        case EVR_UN:  // unknown value representation
          throw OrthancException(ErrorCode_ParameterOutOfRange);


        /**
         * String types.
         **/
      
        case EVR_DS:  // decimal string
        case EVR_IS:  // integer string
        case EVR_AS:  // age string
        case EVR_DA:  // date string
        case EVR_DT:  // date time string
        case EVR_TM:  // time string
        case EVR_AE:  // application entity title
        case EVR_CS:  // code string
        case EVR_SH:  // short string
        case EVR_LO:  // long string
        case EVR_ST:  // short text
        case EVR_LT:  // long text
        case EVR_UT:  // unlimited text
        case EVR_PN:  // person name
        case EVR_UI:  // unique identifier
#if DCMTK_VERSION_NUMBER >= 361
        case EVR_UC:  // unlimited characters
        case EVR_UR:  // URI/URL
#endif
        {
          ok = element.putString(decoded->c_str()).good();
          break;
        }

        
        /**
         * Numerical types
         **/ 
      
        case EVR_SL:  // signed long
        {
          ok = element.putSint32(boost::lexical_cast<Sint32>(*decoded)).good();
          break;
        }

        case EVR_SS:  // signed short
        {
          ok = element.putSint16(boost::lexical_cast<Sint16>(*decoded)).good();
          break;
        }

        case EVR_UL:  // unsigned long
        {
          ok = element.putUint32(boost::lexical_cast<Uint32>(*decoded)).good();
          break;
        }

        case EVR_US:  // unsigned short
        {
          ok = element.putUint16(boost::lexical_cast<Uint16>(*decoded)).good();
          break;
        }

        case EVR_FL:  // float single-precision
        {
          ok = element.putFloat32(boost::lexical_cast<float>(*decoded)).good();
          break;
        }

        case EVR_FD:  // float double-precision
        {
          ok = element.putFloat64(boost::lexical_cast<double>(*decoded)).good();
          break;
        }


        /**
         * Sequence types, should never occur at this point.
         **/

        case EVR_SQ:  // sequence of items
        {
          ok = false;
          break;
        }


        /**
         * Internal to DCMTK.
         **/ 

        case EVR_ox:  // OB or OW depending on context
        case EVR_xs:  // SS or US depending on context
        case EVR_lt:  // US, SS or OW depending on context, used for LUT Data (thus the name)
        case EVR_na:  // na="not applicable", for data which has no VR
        case EVR_up:  // up="unsigned pointer", used internally for DICOMDIR suppor
        case EVR_item:  // used internally for items
        case EVR_metainfo:  // used internally for meta info datasets
        case EVR_dataset:  // used internally for datasets
        case EVR_fileFormat:  // used internally for DICOM files
        case EVR_dicomDir:  // used internally for DICOMDIR objects
        case EVR_dirRecord:  // used internally for DICOMDIR records
        case EVR_pixelSQ:  // used internally for pixel sequences in a compressed image
        case EVR_pixelItem:  // used internally for pixel items in a compressed image
        case EVR_UNKNOWN: // used internally for elements with unknown VR (encoded with 4-byte length field in explicit VR)
        case EVR_PixelData:  // used internally for uncompressed pixeld data
        case EVR_OverlayData:  // used internally for overlay data
        case EVR_UNKNOWN2B:  // used internally for elements with unknown VR with 2-byte length field in explicit VR
        default:
          break;
      }
    }
    catch (boost::bad_lexical_cast&)
    {
      ok = false;
    }

    if (!ok)
    {
      throw OrthancException(ErrorCode_BadFileFormat,
                             "While creating a DICOM instance, tag (" + tag.Format() +
                             ") has out-of-range value: \"" + (*decoded) + "\"");
    }
  }


  DcmElement* FromDcmtkBridge::FromJson(const DicomTag& tag,
                                        const Json::Value& value,
                                        bool decodeDataUriScheme,
                                        Encoding dicomEncoding)
  {
    std::auto_ptr<DcmElement> element;

    switch (value.type())
    {
      case Json::stringValue:
        element.reset(CreateElementForTag(tag));
        FillElementWithString(*element, tag, value.asString(), decodeDataUriScheme, dicomEncoding);
        break;

      case Json::nullValue:
        element.reset(CreateElementForTag(tag));
        FillElementWithString(*element, tag, "", decodeDataUriScheme, dicomEncoding);
        break;

      case Json::arrayValue:
      {
        DcmTag key(tag.GetGroup(), tag.GetElement());
        if (key.getEVR() != EVR_SQ)
        {
          throw OrthancException(ErrorCode_BadParameterType);
        }

        DcmSequenceOfItems* sequence = new DcmSequenceOfItems(key);
        element.reset(sequence);
        
        for (Json::Value::ArrayIndex i = 0; i < value.size(); i++)
        {
          std::auto_ptr<DcmItem> item(new DcmItem);

          switch (value[i].type())
          {
            case Json::objectValue:
            {
              Json::Value::Members members = value[i].getMemberNames();
              for (Json::Value::ArrayIndex j = 0; j < members.size(); j++)
              {
                item->insert(FromJson(ParseTag(members[j]), value[i][members[j]], decodeDataUriScheme, dicomEncoding));
              }
              break;
            }

            case Json::arrayValue:
            {
              // Lua cannot disambiguate between an empty dictionary
              // and an empty array
              if (value[i].size() != 0)
              {
                throw OrthancException(ErrorCode_BadParameterType);
              }
              break;
            }

            default:
              throw OrthancException(ErrorCode_BadParameterType);
          }

          sequence->append(item.release());
        }

        break;
      }

      default:
        throw OrthancException(ErrorCode_BadParameterType);
    }

    return element.release();
  }


  DcmPixelSequence* FromDcmtkBridge::GetPixelSequence(DcmDataset& dataset)
  {
    DcmElement *element = NULL;
    if (!dataset.findAndGetElement(DCM_PixelData, element).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    DcmPixelData& pixelData = dynamic_cast<DcmPixelData&>(*element);
    DcmPixelSequence* pixelSequence = NULL;
    if (!pixelData.getEncapsulatedRepresentation
        (dataset.getOriginalXfer(), NULL, pixelSequence).good())
    {
      return NULL;
    }
    else
    {
      return pixelSequence;
    }
  }


  Encoding FromDcmtkBridge::ExtractEncoding(const Json::Value& json,
                                            Encoding defaultEncoding)
  {
    if (json.type() != Json::objectValue)
    {
      throw OrthancException(ErrorCode_BadParameterType);
    }

    Encoding encoding = defaultEncoding;

    const Json::Value::Members tags = json.getMemberNames();
    
    // Look for SpecificCharacterSet (0008,0005) in the JSON file
    for (size_t i = 0; i < tags.size(); i++)
    {
      DicomTag tag = FromDcmtkBridge::ParseTag(tags[i]);
      if (tag == DICOM_TAG_SPECIFIC_CHARACTER_SET)
      {
        const Json::Value& value = json[tags[i]];
        if (value.type() != Json::stringValue ||
            (value.asString().length() != 0 &&
             !GetDicomEncoding(encoding, value.asCString())))
        {
          throw OrthancException(ErrorCode_BadRequest,
                                 "Unknown encoding while creating DICOM from JSON: " +
                                 value.toStyledString());
        }

        if (value.asString().length() == 0)
        {
          return defaultEncoding;
        }
      }
    }

    return encoding;
  } 


  static void SetString(DcmDataset& target,
                        const DcmTag& tag,
                        const std::string& value)
  {
    if (!target.putAndInsertString(tag, value.c_str()).good())
    {
      throw OrthancException(ErrorCode_InternalError);
    }
  }


  DcmDataset* FromDcmtkBridge::FromJson(const Json::Value& json,  // Encoded using UTF-8
                                        bool generateIdentifiers,
                                        bool decodeDataUriScheme,
                                        Encoding defaultEncoding)
  {
    std::auto_ptr<DcmDataset> result(new DcmDataset);
    Encoding encoding = ExtractEncoding(json, defaultEncoding);

    SetString(*result, DCM_SpecificCharacterSet, GetDicomSpecificCharacterSet(encoding));

    const Json::Value::Members tags = json.getMemberNames();
    
    bool hasPatientId = false;
    bool hasStudyInstanceUid = false;
    bool hasSeriesInstanceUid = false;
    bool hasSopInstanceUid = false;

    for (size_t i = 0; i < tags.size(); i++)
    {
      DicomTag tag = FromDcmtkBridge::ParseTag(tags[i]);
      const Json::Value& value = json[tags[i]];

      if (tag == DICOM_TAG_PATIENT_ID)
      {
        hasPatientId = true;
      }
      else if (tag == DICOM_TAG_STUDY_INSTANCE_UID)
      {
        hasStudyInstanceUid = true;
      }
      else if (tag == DICOM_TAG_SERIES_INSTANCE_UID)
      {
        hasSeriesInstanceUid = true;
      }
      else if (tag == DICOM_TAG_SOP_INSTANCE_UID)
      {
        hasSopInstanceUid = true;
      }

      if (tag != DICOM_TAG_SPECIFIC_CHARACTER_SET)
      {
        std::auto_ptr<DcmElement> element(FromDcmtkBridge::FromJson(tag, value, decodeDataUriScheme, encoding));
        const DcmTagKey& tag = element->getTag();

        result->findAndDeleteElement(tag);

        DcmElement* tmp = element.release();
        if (!result->insert(tmp, false, false).good())
        {
          delete tmp;
          throw OrthancException(ErrorCode_InternalError);
        }
      }
    }

    if (!hasPatientId &&
        generateIdentifiers)
    {
      SetString(*result, DCM_PatientID, GenerateUniqueIdentifier(ResourceType_Patient));
    }

    if (!hasStudyInstanceUid &&
        generateIdentifiers)
    {
      SetString(*result, DCM_StudyInstanceUID, GenerateUniqueIdentifier(ResourceType_Study));
    }

    if (!hasSeriesInstanceUid &&
        generateIdentifiers)
    {
      SetString(*result, DCM_SeriesInstanceUID, GenerateUniqueIdentifier(ResourceType_Series));
    }

    if (!hasSopInstanceUid &&
        generateIdentifiers)
    {
      SetString(*result, DCM_SOPInstanceUID, GenerateUniqueIdentifier(ResourceType_Instance));
    }

    return result.release();
  }


  DcmFileFormat* FromDcmtkBridge::LoadFromMemoryBuffer(const void* buffer,
                                                       size_t size)
  {
    DcmInputBufferStream is;
    if (size > 0)
    {
      is.setBuffer(buffer, size);
    }
    is.setEos();

    std::auto_ptr<DcmFileFormat> result(new DcmFileFormat);

    result->transferInit();
    if (!result->read(is).good())
    {
      throw OrthancException(ErrorCode_BadFileFormat,
                             "Cannot parse an invalid DICOM file (size: " +
                             boost::lexical_cast<std::string>(size) + " bytes)");
    }

    result->loadAllDataIntoMemory();
    result->transferEnd();

    return result.release();
  }


  void FromDcmtkBridge::FromJson(DicomMap& target,
                                 const Json::Value& source)
  {
    if (source.type() != Json::objectValue)
    {
      throw OrthancException(ErrorCode_BadFileFormat);
    }

    target.Clear();

    Json::Value::Members members = source.getMemberNames();

    for (size_t i = 0; i < members.size(); i++)
    {
      const Json::Value& value = source[members[i]];

      if (value.type() != Json::stringValue)
      {
        throw OrthancException(ErrorCode_BadFileFormat);
      }
      
      target.SetValue(ParseTag(members[i]), value.asString(), false);
    }
  }


  void FromDcmtkBridge::ChangeStringEncoding(DcmItem& dataset,
                                             Encoding source,
                                             bool hasSourceCodeExtensions,
                                             Encoding target)
  {
    // Recursive exploration of a dataset to change the encoding of
    // each string-like element

    if (source == target)
    {
      return;
    }

    for (unsigned long i = 0; i < dataset.card(); i++)
    {
      DcmElement* element = dataset.getElement(i);
      if (element)
      {
        if (element->isLeaf())
        {
          char *c = NULL;
          if (element->isaString() &&
              element->getString(c).good() && 
              c != NULL)
          {
            std::string a = Toolbox::ConvertToUtf8(c, source, hasSourceCodeExtensions);
            std::string b = Toolbox::ConvertFromUtf8(a, target);
            element->putString(b.c_str());
          }
        }
        else
        {
          // "All subclasses of DcmElement except for DcmSequenceOfItems
          // are leaf nodes, while DcmSequenceOfItems, DcmItem, DcmDataset
          // etc. are not." The following dynamic_cast is thus OK.
          DcmSequenceOfItems& sequence = dynamic_cast<DcmSequenceOfItems&>(*element);

          for (unsigned long j = 0; j < sequence.card(); j++)
          {
            ChangeStringEncoding(*sequence.getItem(j), source, hasSourceCodeExtensions, target);
          }
        }
      }
    }
  }


  bool FromDcmtkBridge::LookupTransferSyntax(std::string& result,
                                             DcmFileFormat& dicom)
  {
    const char* value = NULL;

    if (dicom.getMetaInfo() != NULL &&
        dicom.getMetaInfo()->findAndGetString(DCM_TransferSyntaxUID, value).good() &&
        value != NULL)
    {
      result.assign(value);
      return true;
    }
    else
    {
      return false;
    }
  }


#if ORTHANC_ENABLE_LUA == 1
  void FromDcmtkBridge::ExecuteToDicom(DicomMap& target,
                                       LuaFunctionCall& call)
  {
    Json::Value output;
    call.ExecuteToJson(output, true /* keep strings */);

    target.Clear();

    if (output.type() == Json::arrayValue &&
        output.size() == 0)
    {
      // This case happens for empty tables
      return;
    }

    if (output.type() != Json::objectValue)
    {
      throw OrthancException(ErrorCode_LuaBadOutput,
                             "Lua: The script must return a table");
    }

    Json::Value::Members members = output.getMemberNames();

    for (size_t i = 0; i < members.size(); i++)
    {
      if (output[members[i]].type() != Json::stringValue)
      {
        throw OrthancException(ErrorCode_LuaBadOutput,
                               "Lua: The script must return a table "
                               "mapping names of DICOM tags to strings");
      }

      DicomTag tag(ParseTag(members[i]));
      target.SetValue(tag, output[members[i]].asString(), false);
    }
  }
#endif


  void FromDcmtkBridge::ExtractDicomSummary(DicomMap& target, 
                                            DcmItem& dataset)
  {
    ExtractDicomSummary(target, dataset,
                        ORTHANC_MAXIMUM_TAG_LENGTH,
                        GetDefaultDicomEncoding());
  }

  
  void FromDcmtkBridge::ExtractDicomAsJson(Json::Value& target, 
                                           DcmDataset& dataset,
                                           const std::set<DicomTag>& ignoreTagLength)
  {
    ExtractDicomAsJson(target, dataset, 
                       DicomToJsonFormat_Full,
                       DicomToJsonFlags_Default, 
                       ORTHANC_MAXIMUM_TAG_LENGTH,
                       GetDefaultDicomEncoding(),
                       ignoreTagLength);
  }


  void FromDcmtkBridge::InitializeCodecs()
  {
#if ORTHANC_ENABLE_DCMTK_JPEG_LOSSLESS == 1
    LOG(INFO) << "Registering JPEG Lossless codecs in DCMTK";
    DJLSDecoderRegistration::registerCodecs();    
#endif

#if ORTHANC_ENABLE_DCMTK_JPEG == 1
    LOG(INFO) << "Registering JPEG codecs in DCMTK";
    DJDecoderRegistration::registerCodecs(); 
#endif
  }


  void FromDcmtkBridge::FinalizeCodecs()
  {
#if ORTHANC_ENABLE_DCMTK_JPEG_LOSSLESS == 1
    // Unregister JPEG-LS codecs
    DJLSDecoderRegistration::cleanup();
#endif

#if ORTHANC_ENABLE_DCMTK_JPEG == 1
    // Unregister JPEG codecs
    DJDecoderRegistration::cleanup();
#endif
  }



  // Forward declaration
  static void ApplyVisitorToElement(DcmElement& element,
                                    ITagVisitor& visitor,
                                    const std::vector<DicomTag>& parentTags,
                                    const std::vector<size_t>& parentIndexes,
                                    Encoding encoding,
                                    bool hasCodeExtensions);
 
  static void ApplyVisitorToDataset(DcmItem& dataset,
                                    ITagVisitor& visitor,
                                    const std::vector<DicomTag>& parentTags,
                                    const std::vector<size_t>& parentIndexes,
                                    Encoding encoding,
                                    bool hasCodeExtensions)
  {
    assert(parentTags.size() == parentIndexes.size());

    for (unsigned long i = 0; i < dataset.card(); i++)
    {
      DcmElement* element = dataset.getElement(i);
      if (element == NULL)
      {
        throw OrthancException(ErrorCode_InternalError);
      }
      else
      {
        ApplyVisitorToElement(*element, visitor, parentTags, parentIndexes, encoding, hasCodeExtensions);
      }      
    }
  }


  static void ApplyVisitorToLeaf(DcmElement& element,
                                 ITagVisitor& visitor,
                                 const std::vector<DicomTag>& parentTags,
                                 const std::vector<size_t>& parentIndexes,
                                 const DicomTag& tag,
                                 Encoding encoding,
                                 bool hasCodeExtensions)
  {
    // TODO - Merge this function, that is more recent, with ConvertLeafElement()

    assert(element.isLeaf());

    DcmEVR evr = element.getTag().getEVR();

    
    /**
     * Fix the EVR for types internal to DCMTK 
     **/

    if (evr == EVR_ox)  // OB or OW depending on context
    {
      evr = EVR_OB;
    }

    if (evr == EVR_UNKNOWN ||  // used internally for elements with unknown VR (encoded with 4-byte length field in explicit VR)
        evr == EVR_UNKNOWN2B)  // used internally for elements with unknown VR with 2-byte length field in explicit VR
    {
      evr = EVR_UN;
    }

    const ValueRepresentation vr = FromDcmtkBridge::Convert(evr);

    
    /**
     * Deal with binary data (including PixelData).
     **/

    if (evr == EVR_OB ||  // other byte
        evr == EVR_OF ||  // other float
#if DCMTK_VERSION_NUMBER >= 361
        evr == EVR_OD ||  // other double
#endif
#if DCMTK_VERSION_NUMBER >= 362
        evr == EVR_OL ||  // other long
#endif
        evr == EVR_OW ||  // other word
        evr == EVR_UN)    // unknown value representation
    {
      Uint16* data16 = NULL;
      Uint8* data = NULL;

      if (evr == EVR_OW &&
          element.getUint16Array(data16) == EC_Normal)
      {
        visitor.VisitBinary(parentTags, parentIndexes, tag, vr, data16, element.getLength());
      }
      else if (evr != EVR_OW &&
               element.getUint8Array(data) == EC_Normal)
      {
        visitor.VisitBinary(parentTags, parentIndexes, tag, vr, data, element.getLength());
      }
      else
      {
        visitor.VisitNotSupported(parentTags, parentIndexes, tag, vr);
      }

      return;  // We're done
    }


    /**
     * Deal with plain strings (and convert them to UTF-8)
     **/

    char *c = NULL;
    if (element.isaString() &&
        element.getString(c).good())
    {
      std::string utf8;

      if (c != NULL)  // This case corresponds to the empty string
      {
        std::string s(c);
        utf8 = Toolbox::ConvertToUtf8(s, encoding, hasCodeExtensions);
      }

      std::string newValue;
      ITagVisitor::Action action = visitor.VisitString
        (newValue, parentTags, parentIndexes, tag, vr, utf8);

      switch (action)
      {
        case ITagVisitor::Action_None:
          break;

        case ITagVisitor::Action_Replace:
        {
          std::string s = Toolbox::ConvertFromUtf8(newValue, encoding);
          if (element.putString(s.c_str()) != EC_Normal)
          {
            throw OrthancException(ErrorCode_InternalError,
                                   "Cannot replace value of tag: " + tag.Format());
          }

          break;
        }

        default:
          throw OrthancException(ErrorCode_InternalError);
      }

      return;  // We're done
    }


    try
    {
      // http://support.dcmtk.org/docs/dcvr_8h-source.html
      switch (evr)
      {

        /**
         * Plain string values.
         **/

        case EVR_DS:  // decimal string
        case EVR_IS:  // integer string
        case EVR_AS:  // age string
        case EVR_DA:  // date string
        case EVR_DT:  // date time string
        case EVR_TM:  // time string
        case EVR_AE:  // application entity title
        case EVR_CS:  // code string
        case EVR_SH:  // short string
        case EVR_LO:  // long string
        case EVR_ST:  // short text
        case EVR_LT:  // long text
        case EVR_UT:  // unlimited text
        case EVR_PN:  // person name
        case EVR_UI:  // unique identifier
        {
          Uint8* data = NULL;

          if (element.getUint8Array(data) == EC_Normal)
          {
            const Uint32 length = element.getLength();
            Uint32 l = 0;
            while (l < length &&
                   data[l] != 0)
            {
              l++;
            }

            if (l == length)
            {
              // Not a null-terminated plain string
              visitor.VisitNotSupported(parentTags, parentIndexes, tag, vr);
            }
            else
            {
              std::string ignored;
              std::string s(reinterpret_cast<const char*>(data), l);
              ITagVisitor::Action action = visitor.VisitString
                (ignored, parentTags, parentIndexes, tag, vr,
                 Toolbox::ConvertToUtf8(s, encoding, hasCodeExtensions));

              if (action != ITagVisitor::Action_None)
              {
                LOG(WARNING) << "Cannot replace this string tag: "
                             << FromDcmtkBridge::GetTagName(element)
                             << " (" << tag.Format() << ")";
              }
            }
          }
          else
          {
            visitor.VisitNotSupported(parentTags, parentIndexes, tag, vr);
          }

          return;
        }
    
        /**
         * Numeric types
         **/ 
      
        case EVR_SL:  // signed long
        {
          DcmSignedLong& content = dynamic_cast<DcmSignedLong&>(element);

          std::vector<int64_t> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Sint32 f;
            if (content.getSint32(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitIntegers(parentTags, parentIndexes, tag, vr, values);
          break;
        }

        case EVR_SS:  // signed short
        {
          DcmSignedShort& content = dynamic_cast<DcmSignedShort&>(element);

          std::vector<int64_t> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Sint16 f;
            if (content.getSint16(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitIntegers(parentTags, parentIndexes, tag, vr, values);
          break;
        }

        case EVR_UL:  // unsigned long
        {
          DcmUnsignedLong& content = dynamic_cast<DcmUnsignedLong&>(element);

          std::vector<int64_t> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Uint32 f;
            if (content.getUint32(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitIntegers(parentTags, parentIndexes, tag, vr, values);
          break;
        }

        case EVR_US:  // unsigned short
        {
          DcmUnsignedShort& content = dynamic_cast<DcmUnsignedShort&>(element);

          std::vector<int64_t> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Uint16 f;
            if (content.getUint16(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitIntegers(parentTags, parentIndexes, tag, vr, values);
          break;
        }

        case EVR_FL:  // float single-precision
        {
          DcmFloatingPointSingle& content = dynamic_cast<DcmFloatingPointSingle&>(element);

          std::vector<double> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Float32 f;
            if (content.getFloat32(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitDoubles(parentTags, parentIndexes, tag, vr, values);
          break;
        }

        case EVR_FD:  // float double-precision
        {
          DcmFloatingPointDouble& content = dynamic_cast<DcmFloatingPointDouble&>(element);

          std::vector<double> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            Float64 f;
            if (content.getFloat64(f, i).good())
            {
              values.push_back(f);
            }
          }

          visitor.VisitDoubles(parentTags, parentIndexes, tag, vr, values);
          break;
        }


        /**
         * Attribute tag.
         **/

        case EVR_AT:
        {
          DcmAttributeTag& content = dynamic_cast<DcmAttributeTag&>(element);

          std::vector<DicomTag> values;
          values.reserve(content.getVM());

          for (unsigned long i = 0; i < content.getVM(); i++)
          {
            DcmTagKey f;
            if (content.getTagVal(f, i).good())
            {
              DicomTag t(f.getGroup(), f.getElement());
              values.push_back(t);
            }
          }

          assert(vr == ValueRepresentation_AttributeTag);
          visitor.VisitAttributes(parentTags, parentIndexes, tag, values);
          break;
        }


        /**
         * Sequence types, should never occur at this point because of
         * "element.isLeaf()".
         **/

        case EVR_SQ:  // sequence of items
        {
          return;
        }
        
        
        /**
         * Internal to DCMTK.
         **/ 

        case EVR_xs:  // SS or US depending on context
        case EVR_lt:  // US, SS or OW depending on context, used for LUT Data (thus the name)
        case EVR_na:  // na="not applicable", for data which has no VR
        case EVR_up:  // up="unsigned pointer", used internally for DICOMDIR suppor
        case EVR_item:  // used internally for items
        case EVR_metainfo:  // used internally for meta info datasets
        case EVR_dataset:  // used internally for datasets
        case EVR_fileFormat:  // used internally for DICOM files
        case EVR_dicomDir:  // used internally for DICOMDIR objects
        case EVR_dirRecord:  // used internally for DICOMDIR records
        case EVR_pixelSQ:  // used internally for pixel sequences in a compressed image
        case EVR_pixelItem:  // used internally for pixel items in a compressed image
        case EVR_PixelData:  // used internally for uncompressed pixeld data
        case EVR_OverlayData:  // used internally for overlay data
        {
          visitor.VisitNotSupported(parentTags, parentIndexes, tag, vr);
          return;
        }
        

        /**
         * Default case.
         **/ 

        default:
          return;
      }
    }
    catch (boost::bad_lexical_cast&)
    {
      return;
    }
    catch (std::bad_cast&)
    {
      return;
    }
  }


  static void ApplyVisitorToElement(DcmElement& element,
                                    ITagVisitor& visitor,
                                    const std::vector<DicomTag>& parentTags,
                                    const std::vector<size_t>& parentIndexes,
                                    Encoding encoding,
                                    bool hasCodeExtensions)
  {
    assert(parentTags.size() == parentIndexes.size());

    DicomTag tag(FromDcmtkBridge::Convert(element.getTag()));

    if (element.isLeaf())
    {
      ApplyVisitorToLeaf(element, visitor, parentTags, parentIndexes, tag, encoding, hasCodeExtensions);
    }
    else
    {
      // "All subclasses of DcmElement except for DcmSequenceOfItems
      // are leaf nodes, while DcmSequenceOfItems, DcmItem, DcmDataset
      // etc. are not." The following dynamic_cast is thus OK.
      DcmSequenceOfItems& sequence = dynamic_cast<DcmSequenceOfItems&>(element);

      if (sequence.card() == 0)
      {
        visitor.VisitEmptySequence(parentTags, parentIndexes, tag);
      }
      else
      {
        std::vector<DicomTag> tags = parentTags;
        std::vector<size_t> indexes = parentIndexes;
        tags.push_back(tag);
        indexes.push_back(0);

        for (unsigned long i = 0; i < sequence.card(); i++)
        {
          indexes.back() = static_cast<size_t>(i);
          DcmItem* child = sequence.getItem(i);
          ApplyVisitorToDataset(*child, visitor, tags, indexes, encoding, hasCodeExtensions);
        }
      }
    }
  }


  void FromDcmtkBridge::Apply(DcmItem& dataset,
                              ITagVisitor& visitor,
                              Encoding defaultEncoding)
  {
    std::vector<DicomTag> parentTags;
    std::vector<size_t> parentIndexes;
    bool hasCodeExtensions;
    Encoding encoding = DetectEncoding(hasCodeExtensions, dataset, defaultEncoding);
    ApplyVisitorToDataset(dataset, visitor, parentTags, parentIndexes, encoding, hasCodeExtensions);
  }
}
