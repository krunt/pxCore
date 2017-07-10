/*

 rtCore Copyright 2005-2017 John Robinson

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

// rtHttpCache.cpp

#include <rtHttpCache.h>
#include <string.h>
#include <sstream>
#include "rtLog.h"
#include <rtFileDownloader.h>

#if !defined(WIN32) && !defined(ENABLE_DFB)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#endif

#include <curl/curl.h>

#if !defined(WIN32) && !defined(ENABLE_DFB)
#pragma GCC diagnostic pop
#endif

using namespace std;

rtHttpCacheData::rtHttpCacheData():mExpirationDate(0),mUpdated(false)
{
  fp = NULL;
}

rtHttpCacheData::rtHttpCacheData(const char* url):mUrl(url),mExpirationDate(0),mUpdated(false)
{
  fp = NULL;
}

rtHttpCacheData::rtHttpCacheData(const char* url, const char* headerMetadata, const char* data, int size):mUrl(url),mExpirationDate(0),mUpdated(false)
{
  if ((NULL != headerMetadata) && (NULL != data))
  {
    mHeaderMetaData.init((uint8_t *)headerMetadata,strlen(headerMetadata));
    populateHeaderMap();
    setExpirationDate();
    mData.init((uint8_t *)data,size);
  }
  fp = NULL;
}

rtHttpCacheData::~rtHttpCacheData()
{
  fp = NULL;
}

void rtHttpCacheData::populateHeaderMap()
{
  size_t pos=0,prevpos = 0;
  string headerString((char*)mHeaderMetaData.data());
  pos = headerString.find_first_of("\n",0);
  string attribute("");
  while (pos !=  string::npos)
  {
    attribute = headerString.substr(prevpos,pos-prevpos);
    if (attribute.size() >  0)
    {
      //parsing the header attribute and value pair
      string key(""),value("");
      size_t name_end_pos = attribute.find_first_of(":");
      if (name_end_pos == string::npos)
      {
        key = attribute;
      }
      else
      {
        key = attribute.substr(0,name_end_pos);
      }
      size_t cReturn_nwLnPos  = key.find_first_of("\r");
      if (string::npos != cReturn_nwLnPos)
        key.erase(cReturn_nwLnPos,1);
      cReturn_nwLnPos  = key.find_first_of("\n");
      if (string::npos != cReturn_nwLnPos)
        key.erase(cReturn_nwLnPos,1);
      if (name_end_pos == string::npos)
      {
        if (key.size() > 0)
          mHeaderMap.insert(std::pair<rtString, rtString>(key.c_str(),rtString("")));
      }
      else
      {
       value = attribute.substr(name_end_pos+1,attribute.length());
       cReturn_nwLnPos  = value.find_first_of("\r");
       if (string::npos != cReturn_nwLnPos)
         value.erase(cReturn_nwLnPos,1);
       cReturn_nwLnPos  = value.find_first_of("\n");
       if (string::npos != cReturn_nwLnPos)
         value.erase(cReturn_nwLnPos,1);
       if (key.size() > 0)
         mHeaderMap.insert(std::pair<rtString, rtString>(key.c_str(),value.c_str()));
      }
    }
    prevpos = pos+1;
    pos = headerString.find_first_of("\n",prevpos);
  }
}

rtString rtHttpCacheData::expirationDate()
{
  char buffer[100];
  memset(buffer,0,100);
  strftime(buffer, 100, "%Y-%m-%d %H:%M:%S", localtime(&mExpirationDate));
  return rtString(buffer);
}

time_t rtHttpCacheData::expirationDateUnix() const
{
  return mExpirationDate;
}

bool rtHttpCacheData::isExpired()
{
  if (0 == mExpirationDate)
    return true;

  time_t now = time(NULL);
  if (now >= mExpirationDate)
    return true;
  return false;
}

bool rtHttpCacheData::isValid()
{
  if ((mData.length() != 0)  && !isExpired())
  {
    return true;
  }
  return false;
}

bool rtHttpCacheData::isWritableToCache()
{
  // need to add more  conditions ???
  if (isValid())
  {
    string cacheControl = mHeaderMap["Cache-Control"].cString();
    if (string::npos != cacheControl.find("no-store"))
    {
      return false;
    }
    return true;
  }
  return false;
}

void rtHttpCacheData::setAttributes(char* rawAttributes)
{
  mHeaderMetaData.init((uint8_t*)rawAttributes,strlen(rawAttributes));
  populateHeaderMap();
  setExpirationDate();
}

rtError rtHttpCacheData::attributes(map<rtString, rtString>& cacheAttributes)
{
  cacheAttributes = mHeaderMap;
  return RT_OK;
}

rtData& rtHttpCacheData::headerData()
{
  return mHeaderMetaData;
}

rtData& rtHttpCacheData::contentsData()
{
  return mData;
}

rtError rtHttpCacheData::data(rtData& data)
{
  if (NULL == fp)
    return RT_ERROR;

  populateExpirationDateFromCache();

  bool revalidate =  false;
  bool revalidateOnlyHeaders = false;

  rtError res;
  res = calculateRevalidationNeed(revalidate,revalidateOnlyHeaders);

  if (RT_OK != res)
    return res;

  if (true == revalidate)
    return performRevalidation(data);

  if (true == revalidateOnlyHeaders)
  {
    if (RT_OK != performHeaderRevalidation())
      return RT_ERROR;
  }

  if (mHeaderMap.end() != mHeaderMap.find("ETag"))
  {
    rtError res =  handleEtag(data);
    if (RT_OK != res)
      return RT_ERROR;
    if (mUpdated)
    {
      return RT_OK;
    }
  }

  if (false == readFileData())
    return RT_ERROR;

  data.init(mData.data(),mData.length());
  if (true == revalidateOnlyHeaders)
  {
    mUpdated = true; //headers  modified , so rewriting the cache with new header data
  }
  return RT_OK;
}

void rtHttpCacheData::setData(rtData& cacheData)
{
  mData.init(cacheData.data(),cacheData.length());
}

rtError rtHttpCacheData::url(rtString& url) const
{
  url = mUrl;
  return RT_OK;
}

rtError rtHttpCacheData::etag(rtString& tag) //returns the etag (if available)
{
  if (mHeaderMap.end() != mHeaderMap.find("ETag"))
  {
    tag = mHeaderMap["ETag"];
    return RT_OK;
  }
  return RT_ERROR;
}

bool rtHttpCacheData::isUpdated()
{
  return mUpdated;
}

void rtHttpCacheData::setFilePointer(FILE* openedDescriptor)
{
  fp = openedDescriptor;
}

void rtHttpCacheData::setExpirationDate()
{
  string expirationDate = "";
  bool foundMaxAge = false;
  if (mHeaderMap.end() != mHeaderMap.find("Cache-Control"))
  {
    string cacheControl = mHeaderMap["Cache-Control"].cString();
    size_t pos = cacheControl.find("max-age");
    if (string::npos != pos)
    {
      foundMaxAge = true;
      string maxAge = cacheControl.substr(pos+8);
      long int maxAgeInt = 0;
      stringstream stream(maxAge);
      stream >> maxAgeInt;
      mExpirationDate = time(NULL) + maxAgeInt;
    }
  }
  if (false == foundMaxAge)
  {
    if (mHeaderMap.end() != mHeaderMap.find("Expires"))
    {
      struct tm timeinfo;
      memset(&timeinfo,0,sizeof(struct tm));
      strptime(mHeaderMap["Expires"].cString(), " %a, %d %b %Y %H:%M:%S %Z", &timeinfo);
      mExpirationDate = timegm(&timeinfo);
    }
  }
}

rtError rtHttpCacheData::calculateRevalidationNeed(bool& revalidate, bool& revalidateOnlyHeaders)
{
  if (isExpired())
  {
    if (mHeaderMap.end() != mHeaderMap.find("Cache-Control"))
    {
      string cacheControl = mHeaderMap["Cache-Control"].cString();
      size_t pos = cacheControl.find("must-revalidate");
      if (string::npos != pos)
      {
        revalidate = true;
        return RT_OK;
      }
      else
        return RT_ERROR; //expired cache data and need to be reloaded again
    }
  }

  if (mHeaderMap.end() != mHeaderMap.find("Cache-Control"))
  {
    string cacheControl = mHeaderMap["Cache-Control"].cString();
    size_t pos = 0,prevpos = 0;
    while ((pos = cacheControl.find("no-cache",prevpos)) != string::npos)
    {
       //no-cache=<parameter>
       if ((cacheControl.find("=") != string::npos) && (cacheControl.at(pos+8) == '='))
       {
         size_t noCacheEnd =  cacheControl.find_first_of(",",pos+9);
         string parameter;
         // no-cache can be last parameter
         if (string::npos == noCacheEnd)
         {
           parameter = cacheControl.substr(pos+9);
         }
         else
         {
           parameter = cacheControl.substr(pos+9,noCacheEnd - (pos + 9));
         }
         rtLogWarn("Erasing header [%s] \n",parameter.c_str());
         fflush(stdout);
         mHeaderMap.erase(parameter.c_str());
         revalidateOnlyHeaders = true;
         if (string::npos == noCacheEnd)
           break;
         prevpos = noCacheEnd;
       }
       else
       {
         //Revalidate the full contents, so download it completely newer
         revalidate = true;
         break;
       }
    }
  }
  return RT_OK;
}

bool rtHttpCacheData::handleDownloadRequest(vector<rtString>& headers,bool downloadBody)
{
  rtFileDownloadRequest* downloadRequest = NULL;
  downloadRequest = new rtFileDownloadRequest(mUrl, this);
  downloadRequest->setAdditionalHttpHeaders(headers);

  if (!downloadBody)
    downloadRequest->setHeaderOnly(true);

  if (false == rtFileDownloader::instance()->downloadFromNetwork(downloadRequest))
  {
     delete downloadRequest;
     return false;
  }

  if ((downloadRequest->httpStatusCode() == 404) || (downloadRequest->httpStatusCode() == 403))
  {
    delete downloadRequest;
    return false;
  }

  if (downloadRequest->downloadStatusCode() == 0 &&
       downloadRequest->httpStatusCode() == 200)
  {
    if (downloadRequest->headerData() != NULL)
      mHeaderMetaData.init((uint8_t*)downloadRequest->headerData(), downloadRequest->headerDataSize());
    if (downloadRequest->downloadedData() != NULL)
    {
      mData.init((uint8_t*)downloadRequest->downloadedData(), downloadRequest->downloadedDataSize());
      mUpdated = true;
    }
  }

  delete downloadRequest;
  return true;
}

bool rtHttpCacheData::readFileData()
{
  char *contentsData = NULL;
  char* tmp = NULL;
  char buffer[100];
  int bytesCount = 0;
  int totalBytes = 0;
  while (!feof(fp))
  {
    bytesCount = fread(buffer,1,100,fp);
    if (NULL == contentsData)
      tmp = (char *)malloc(bytesCount);
    else
    {
      tmp = (char *)realloc(contentsData,totalBytes+bytesCount);
    }
    if (NULL == tmp)
    {
      rtLogError("reading the cache data failed due to memory lack \n");
      fflush(stdout);
      fclose(fp);
      if (NULL != contentsData)
        free(contentsData);
      contentsData = NULL;
      return false;
    }
    contentsData = tmp;
    memcpy(contentsData+totalBytes,buffer,bytesCount);
    totalBytes += bytesCount;
    memset(buffer,0,100);
  }
  fclose(fp);
  if (NULL != contentsData)
  {
    mData.init((uint8_t*)contentsData,totalBytes);
    free(contentsData);
    contentsData = NULL;
    tmp = NULL;
  }
  return true;
}

void rtHttpCacheData::populateExpirationDateFromCache()
{
  char buf;
  string date;
  while ( !feof(fp) )
  {
    buf = fgetc(fp);
    if (buf == '|')
    {
      break;
    }
    date.append(1,buf);
  }

  stringstream stream(date);
  stream >> mExpirationDate;
}

rtError rtHttpCacheData::performRevalidation(rtData& data)
{
  rtString headerOption = "Cache-Control: max-age=0";
  vector<rtString> headers;
  headers.push_back(headerOption);

  if (!handleDownloadRequest(headers))
  {
    return RT_ERROR;
  }

  if (mUpdated)
  {
    populateHeaderMap();
    setExpirationDate();
    data.init(mData.data(),mData.length());
    fclose(fp);
    return RT_OK;
  }
  else
  {
    return RT_ERROR;
  }
}

rtError rtHttpCacheData::performHeaderRevalidation()
{
  rtString headerOption = "Cache-Control: max-age=0";
  vector<rtString> headers;
  headers.push_back(headerOption);

  if (!handleDownloadRequest(headers,false))
  {
    return RT_ERROR;
  }

  populateHeaderMap();
  setExpirationDate();
  return RT_OK;
}

rtError rtHttpCacheData::handleEtag(rtData& data)
{
  rtString headerOption = "If-None-Match:";
  headerOption.append(mHeaderMap["ETag"].cString());
  vector<rtString> headers;
  headers.push_back(headerOption);

  if (!handleDownloadRequest(headers))
  {
    return RT_ERROR;
  }

  if (mUpdated)
  {
    populateHeaderMap();
    setExpirationDate();
    data.init(mData.data(),mData.length());
    fclose(fp);
  }
  return RT_OK;
}
