/*
 * SessionRmdNotebook.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"
#include "SessionRnbParser.hpp"

#include <iostream>

#include <boost/foreach.hpp>

#include <r/RJson.hpp>
#include <r/RExec.hpp>
#include <r/RRoutines.hpp>

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/Algorithm.hpp>
#include <core/json/Json.hpp>
#include <core/json/JsonRpc.hpp>
#include <core/text/CsvParser.hpp>
#include <core/StringUtils.hpp>
#include <core/system/System.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/SessionSourceDatabase.hpp>
#include <session/SessionOptions.hpp>
#include <session/SessionUserSettings.hpp>

#define kChunkDefs         "chunk_definitions"
#define kChunkDocWriteTime "doc_write_time"
#define kChunkDocId        "doc_id"
#define kChunkId           "chunk_id"
#define kChunkOutputPath   "chunk_output"
#define kChunkUrl          "url"
#define kChunkConsole      "console"

#define kChunkConsoleInput  0
#define kChunkConsoleOutput 1
#define kChunkConsoleError  3

#define kChunkTypeNone    0
#define kChunkTypeOutput  1
#define kChunkTypeConsole 2

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

namespace {

// the ID of the doc / chunk currently executing console commands (if any)
std::string s_consoleChunkId, s_consoleDocId, s_activeConsole;

// whether we're currently connected to console events
bool s_consoleConnected = false;

// A notebook .Rmd is accompanied by a sidecar .Rnb.cached folder, which has
// the following structure:
//
// - foo.Rmd
// + .foo.Rnd.cached
//   - chunks.json
//   - cwiaiw9i4f0.html
//   + cwiaiw9i4f0_files
//     - plot.png
//   - c0aj9vhk0cz.html
//   - cjz0958jgzh.csv
//   + lib
//     + htmlwidgets
//       - htmlwidget.js
// 
// That is:
// - each chunk has an ID and is represented by one of the following:
//   - an HTML file (.html) with accompanying dependencies indicating
//     chunk output, or 
//   - a CSV file (.csv) indicating console output
// - the special file "chunks.json" indicates the location of the chunks
//   in the source .Rmd
// - the special folder "lib" is used for shared libraries (e.g. scripts upon
//   which several htmlwidget chunks depend)


FilePath unsavedNotebookCache()
{
   return module_context::sessionScratchPath().childPath("unsaved-notebooks");
}

FilePath chunkCacheFolder(const std::string& docPath, const std::string& docId,
      const std::string& contextId)
{
   FilePath folder;
   std::string stem;

   if (docPath.empty()) 
   {
      // the doc hasn't been saved, so keep its chunk output in the scratch
      // path
      folder = unsavedNotebookCache();
      stem = docId;
   }
   else
   {
      // the doc has been saved, so keep its chunk output alongside the doc
      // itself
      FilePath path = module_context::resolveAliasedPath(docPath);

#ifndef _WIN32
      // on non-Windows, use unix hidden folder 
      stem = ".";
#endif

      stem += path.stem();
      stem += "-" + contextId;
      folder = path.parent();
   }

   return folder.childPath(stem + ".Rnb.cached");
}

FilePath chunkCacheFolder(const std::string& docPath, const std::string& docId)
{
   return chunkCacheFolder(docPath, docId, userSettings().contextId());
}

FilePath chunkDefinitionsPath(const std::string& docPath,
                              const std::string& docId,
                              const std::string& contextId)
{
   std::string fileName = std::string() + "chunks.json";
   return chunkCacheFolder(docPath, docId, contextId).childPath(fileName);
}

FilePath chunkOutputPath(
      const std::string& docPath, const std::string& docId,
      const std::string& chunkId, const std::string& contextId)

{
   return chunkCacheFolder(docPath, docId, contextId)
                          .childPath(chunkId + ".html");
}

FilePath chunkConsolePath(
      const std::string& docPath, const std::string& docId, 
      const std::string& chunkId, const std::string& contextId)
   
{
   return chunkCacheFolder(docPath, docId, contextId)
                          .childPath(chunkId + ".csv");
}

Error chunkConsoleContents(const std::string& docPath, const std::string& docId, 
      const std::string& chunkId, const std::string& contextId,
      json::Array* pArray)
{
   std::string contents;
   FilePath consoleFile = chunkConsolePath(docPath, docId, chunkId, contextId);
   Error error = readStringFromFile(consoleFile, &contents);
   if (error)
      return error;

   // parse each line of the CSV file
   std::pair<std::vector<std::string>, std::string::iterator> line;
   line = text::parseCsvLine(contents.begin(), contents.end());
   while (!line.first.empty())
   {
      if (line.first.size() > 1)
      {
         json::Array output;
         output.push_back(safe_convert::stringTo<int>(line.first[0], 
               module_context::ConsoleOutputNormal));
         output.push_back(line.first[1]);
         pArray->push_back(output);
      }
      // read next line
      line = text::parseCsvLine(line.second, contents.end());
   }

   return Success();
}


Error enqueueChunkOutput(
      const std::string& docPath, const std::string& docId,
      const std::string& chunkId, const std::string& contextId)
{
   Error error;
   FilePath outputPath = chunkOutputPath(docPath, docId, chunkId, contextId);
   FilePath consolePath = chunkConsolePath(docPath, docId, chunkId, contextId);

   unsigned chunkType = kChunkTypeNone;
   if (outputPath.exists() && !consolePath.exists())
   {
      chunkType = kChunkTypeOutput;
   } 
   else if (consolePath.exists() && !outputPath.exists())
   {
      chunkType = kChunkTypeConsole;
   }
   else if (outputPath.exists() && consolePath.exists())
   {
      // pick the more recent one if we have both
      if (outputPath.lastWriteTime() > consolePath.lastWriteTime())
         chunkType = kChunkTypeOutput;
      else
         chunkType = kChunkTypeConsole;
   }
   json::Object output;
   if (chunkType == kChunkTypeOutput)
   {
      output[kChunkUrl] = kChunkOutputPath "/" + docId + "/" + chunkId + ".html";
   }
   else if (chunkType == kChunkTypeConsole)
   {
      json::Array consoleOutput;
      error = chunkConsoleContents(docPath, docId, chunkId, contextId,
            &consoleOutput);
      if (error)
         LOG_ERROR(error);
      output[kChunkConsole] = consoleOutput;
   }
   // note that if we find that this chunk has no output we can display, we
   // should still send it to the client, which will clean it up correctly, and
   // omit it in its next set of updated chunk definitions
   output[kChunkId] = chunkId;
   output[kChunkDocId] = docId;
   ClientEvent event(client_events::kChunkOutput, output);
   module_context::enqueClientEvent(event);

   return Success();
}

void extractChunkIds(const json::Array& chunkOutputs, 
                     std::vector<std::string> *pIds)
{
   BOOST_FOREACH(const json::Value& chunkOutput, chunkOutputs)
   {
      if (chunkOutput.type() != json::ObjectType)
         continue;
      std::string chunkId;
      if (json::readObject(chunkOutput.get_obj(), kChunkId, &chunkId) ==
            Success()) 
      {
         pIds->push_back(chunkId);
      }
   }
}

void replayChunkOutputs(const std::string& docPath, const std::string& docId,
      const std::string& requestId, const json::Array& chunkOutputs) 
{
   std::vector<std::string> chunkIds;
   extractChunkIds(chunkOutputs, &chunkIds);

   // find all the chunks and play them back to the client
   BOOST_FOREACH(const std::string& chunkId, chunkIds)
   {
      enqueueChunkOutput(docPath, docId, chunkId, userSettings().contextId());
   }

   json::Object result;
   result["path"] = docPath;
   result["request_id"] = requestId;
   ClientEvent event(client_events::kChunkOutputFinished, result);
   module_context::enqueClientEvent(event);
}

Error getChunkDefs(const std::string& docPath, const std::string& docId,
                   const std::string& contextId, time_t *pDocTime, 
                   core::json::Value* pDefs)
{
   Error error;
   FilePath defs = chunkDefinitionsPath(docPath, docId, contextId);
   if (!defs.exists())
      return Success();

   // read the defs file 
   std::string contents;
   error = readStringFromFile(defs, &contents);
   if (error)
      return error;

   // pull out the contents
   json::Value defContents;
   if (!json::parse(contents, &defContents) || 
       defContents.type() != json::ObjectType)
      return Error(json::errc::ParseError, ERROR_LOCATION);

   // extract the chunk definitions
   if (pDefs)
   {
      json::Array chunkDefs;
      error = json::readObject(defContents.get_obj(), kChunkDefs, &chunkDefs);
      if (error)
         return error;

      // return to caller
      *pDefs = chunkDefs;
   }

   // extract the doc write time 
   if (pDocTime)
   {
      json::Object::iterator it = 
         defContents.get_obj().find(kChunkDocWriteTime);
      if (it != defContents.get_obj().end() &&
          it->second.type() == json::IntegerType)
      {
         *pDocTime = static_cast<std::time_t>(it->second.get_int64());
      }
      else
      {
         return Error(json::errc::ParamMissing, ERROR_LOCATION);
      }
   }
   return Success();
}

// called by the client to inject output into a recently opened document 
Error refreshChunkOutput(const json::JsonRpcRequest& request,
                         json::JsonRpcResponse* pResponse)
{
   // extract path to doc to be refreshed
   std::string docPath, docId, contextId, requestId;
   Error error = json::readParams(request.params, &docPath, &docId, &contextId,
         &requestId);
   if (error)
      return error;

   // use our own context ID if none supplied
   if (contextId.empty())
      contextId = userSettings().contextId();

   json::Object result;
   json::Value chunkDefs; 
   error = getChunkDefs(docPath, docId, contextId, NULL, &chunkDefs);

   // schedule the work to play back the chunks
   if (!error && chunkDefs.type() == json::ArrayType) 
   {
      pResponse->setAfterResponse(
            boost::bind(replayChunkOutputs, docPath, docId, requestId, 
                        chunkDefs.get_array()));
   }

   return Success();
}

bool copyCacheItem(const FilePath& from,
                   const FilePath& to,
                   const FilePath& path)
{

   std::string relativePath = path.relativePath(from);
   FilePath target = to.complete(relativePath);

   Error error = path.isDirectory() ?
                     target.ensureDirectory() :
                     path.copy(target);
   if (error)
      LOG_ERROR(error);

   return true;
}

Error copyCache(const FilePath& from, const FilePath& to)
{
   Error error = to.ensureDirectory();
   if (error)
      return error;

   return from.childrenRecursive(
             boost::bind(copyCacheItem, from, to, _2));
}

void onDocRemoved(const std::string& docId, const std::string& docPath)
{
   Error error;

   FilePath cacheFolder = chunkCacheFolder(docPath, docId);
   FilePath defFile = chunkDefinitionsPath(docPath, docId, 
         userSettings().contextId());
   if (!docPath.empty() && defFile.exists())
   {
      // for saved documents, we want to keep the cache folder around even when
      // the document is closed, but only if the chunk definitions aren't out
      // of sync.
      FilePath docFile = module_context::resolveAliasedPath(docPath);
      std::time_t writeTime;
      error = getChunkDefs(docPath, docId, userSettings().contextId(),
            &writeTime, NULL);

      if (writeTime <= docFile.lastWriteTime())
      {
         // the doc has been saved since the last time the chunks defs were
         // updated, so no work to do here
         return;
      }
   }
   error = cacheFolder.removeIfExists();
   if (error)
      LOG_ERROR(error);
}

void onDocRenamed(const std::string& oldPath, 
                  boost::shared_ptr<source_database::SourceDocument> pDoc)
{
   Error error;
   bool removeOldDir = false;

   // compute cache folders and ignore if we can't safely adjust them
   FilePath oldCacheDir = chunkCacheFolder(oldPath, pDoc->id());
   FilePath newCacheDir = chunkCacheFolder(pDoc->path(), pDoc->id());
   if (!oldCacheDir.exists() || newCacheDir.exists())
      return;

   // if the doc was previously unsaved, we can just move the whole folder 
   // to its newly saved location
   if (oldPath.empty())
   {
      error = oldCacheDir.move(newCacheDir);
      if (error) 
      {
         // if we can't move the cache to the new location, we'll fall back to
         // copy/remove
         removeOldDir = true;
      }
      else
         return;
   }

   error = copyCache(oldCacheDir, newCacheDir);
   if (error)
   {
      LOG_ERROR(error);
   }
   else if (removeOldDir) 
   {
      // remove old dir if we couldn't move the folder above
      error = oldCacheDir.remove();
      if (error)
         LOG_ERROR(error);
   }
}

void onDocAdded(const std::string& id)
{
   std::string path;
   Error error = source_database::getPath(id, &path);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // ignore empty paths and non-R Markdown files
   if (path.empty())
      return;
   FilePath docPath = module_context::resolveAliasedPath(path);
   if (docPath.extensionLowerCase() != ".rmd")
      return;

   FilePath cachePath = chunkCacheFolder(path, id);
   FilePath nbPath = docPath.parent().complete(docPath.stem() + ".Rnb");

   if (!cachePath.exists() && nbPath.exists())
   {
      // we have a saved representation, but no cache -- populate the cache
      // from the saved representation
      error = parseRnb(nbPath, cachePath);
      if (error)
      {
         LOG_ERROR(error);
         return;
      }
   }

   // TODO: consider write times of document, cache, and .Rnb -- are there
   // combinations which would suggest we should overwrite the cache with the
   // contents of the notebook?
}

void disconnectConsole();

void onConsolePrompt(const std::string& )
{
   if (s_consoleConnected)
      disconnectConsole();
}

void onConsoleText(const std::string& docId, const std::string& chunkId,
                   int type, const std::string& output, bool truncate)
{
   if (output.empty())
      return;

   FilePath path;
   Error error = source_database::getPath(docId, &path);
   if (error)
   {
      LOG_ERROR(error);
   }

   FilePath outputCsv = chunkConsolePath(path.absolutePath(), 
         docId, chunkId, userSettings().contextId());

   std::vector<std::string> vals; 
   vals.push_back(safe_convert::numberToString(type));
   vals.push_back(output);
   error = core::writeStringToFile(outputCsv, 
         text::encodeCsvLine(vals) + "\n", 
         string_utils::LineEndingPassthrough, truncate);
   if (error)
   {
      LOG_ERROR(error);
   }

   events().onChunkConsoleOutput(docId, chunkId, type, output);
}

void onConsoleOutput(module_context::ConsoleOutputType type, 
      const std::string& output)
{
   if (type == module_context::ConsoleOutputNormal)
      onConsoleText(s_consoleDocId, s_consoleChunkId, kChunkConsoleOutput, 
                    output, false);
   else
      onConsoleText(s_consoleDocId, s_consoleChunkId, kChunkConsoleError, 
                    output, false);
}

void onConsoleInput(const std::string& input)
{
   onConsoleText(s_consoleDocId, s_consoleChunkId, kChunkConsoleInput, input, 
         false);
}

void disconnectConsole()
{
   module_context::events().onConsolePrompt.disconnect(onConsolePrompt);
   module_context::events().onConsoleOutput.disconnect(onConsoleOutput);
   module_context::events().onConsoleInput.disconnect(onConsoleInput);
   s_consoleConnected = false;
}

void connectConsole()
{
   module_context::events().onConsolePrompt.connect(onConsolePrompt);
   module_context::events().onConsoleOutput.connect(onConsoleOutput);
   module_context::events().onConsoleInput.connect(onConsoleInput);
   s_consoleConnected = true;
}

void onActiveConsoleChanged(const std::string& consoleId, 
                            const std::string& text)
{
   s_activeConsole = consoleId;
   if (consoleId == s_consoleChunkId)
   {
      if (s_consoleConnected) 
         return;
      connectConsole();
      onConsoleText(s_consoleDocId, s_consoleChunkId, kChunkConsoleInput, 
            text, false);
   }
   else if (s_consoleConnected)
   {
      // some other console is connected; disconnect ours
      disconnectConsole();
   }
}

void onChunkExecCompleted(const std::string& docId, 
                          const std::string& chunkId,
                          const std::string& contextId)
{
   // attempt to get the path of the doc (this may fail if the document does
   // not yet exist)
   std::string path;
   source_database::getPath(docId, &path);

   Error error = enqueueChunkOutput(path, docId, chunkId, contextId);
   if (error)
      LOG_ERROR(error);
}

void executeSingleInlineChunk(const std::string& docPath, 
                              const std::string& docId, 
                              const std::string& chunkId,
                              const std::string& chunkOptions, 
                              const std::string& content)
{
   Error error;

   // ensure we have a place to put the output
   FilePath chunkOutput = chunkOutputPath(docPath, docId, chunkId,
         userSettings().contextId());
   if (!chunkOutput.parent().exists())
   {
      error = ensureCacheFolder(chunkOutput.parent());
      if (error)
      {
         LOG_ERROR(error);
         return;
      }
   }

   // ensure we have a library path
   FilePath chunkLibDir = chunkCacheFolder(docPath, docId).complete(
         kChunkLibDir);
   error = chunkLibDir.ensureDirectory();
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   FilePath headerHtml = options().rResourcesPath().complete("notebook").
      complete("in_header.html");

   // render the contents to the cached folder
   r::sexp::Protect protect;
   SEXP err;
   error = r::exec::RFunction(".rs.executeSingleChunk", chunkOptions, content,
         chunkLibDir.absolutePath(),
         headerHtml.absolutePath(),
         chunkOutput.absolutePath()).call(&err, &protect);
   if (error)
   {
      onConsoleText(docId, chunkId, kChunkConsoleError, error.summary(), 
            true);
      LOG_ERROR(error);
      return;
   }

   // if an error message was returned, show a console error instead of the
   // output
   std::string errorMsg;
   if (r::sexp::getNamedListElement(err, "message", &errorMsg) == Success() &&
       !errorMsg.empty())
   {
      chunkOutput.removeIfExists();
      onConsoleText(docId, chunkId, kChunkConsoleError, errorMsg + "\n", 
            true);

      // consider: the "text" member of the list element contains the contents
      // of stderr, which may include message such as "Quitting from lines 2-4"
      // which could be used to help the user pinpoint the source of the
      // problem (although these messages currently just point at the current
      // chunk so not helpful without some additional plumbing) 
   }

   // emit chunk output to client
   events().onChunkExecCompleted(docId, chunkId, userSettings().contextId());
}

Error handleChunkOutputRequest(const http::Request& request,
                               http::Response* pResponse)
{
   // uri format is: /chunk_output/<doc-id>/...
   
   // split URI into pieces, extract the document ID, and remove that part of
   // the URI
   std::vector<std::string> parts = algorithm::split(request.uri(), "/");
   if (parts.size() < 4) 
      return Success();
   std::string docId = parts[2];
   for (int i = 0; i < 3; i++)
      parts.erase(parts.begin());

   // attempt to get the path -- ignore failure (doc may be unsaved and
   // therefore won't have a path)
   std::string path;
   source_database::getPath(docId, &path);

   FilePath target = chunkCacheFolder(path, docId).complete(
         algorithm::join(parts, "/"));

   // ensure the target exists 
   if (!target.exists())
   {
      pResponse->setNotFoundError(request.uri());
      return Success();
   }

   if (parts[0] == kChunkLibDir)
   {
      // if a reference to the chunk library folder, we can reuse the contents
      // (let the browser cache the file)
      pResponse->setCacheableFile(target, request);
   }
   else
   {
      // otherwise, use ETag cache 
      pResponse->setCacheableBody(target, request);
   }

   return Success();
}

// called by the client to set the active chunk console
Error setChunkConsole(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse*)
{

   std::string docId, chunkId;
   Error error = json::readParams(request.params, &docId, &chunkId);
   if (error)
      return error;

   s_consoleChunkId = chunkId;
   s_consoleDocId = docId;
   if (s_activeConsole == chunkId)
      connectConsole();

   return Success();
}

// called by client to execute a single chunk in a document
Error executeInlineChunk(const json::JsonRpcRequest& request,
                         json::JsonRpcResponse* pResponse)
{
   std::string docPath, docId, chunkId, chunkOptions, content;
   Error error = json::readParams(request.params, &docPath, &docId, &chunkId, 
         &chunkOptions, &content);
   if (error)
      return error;

   // return immediately from RPC 
   pResponse->setAfterResponse(
      boost::bind(executeSingleInlineChunk, docPath, docId, chunkId, 
                  chunkOptions, content));

   return Success();
}

// given and old and new set of chunk definitions, cleans up all the chunks
// files in the old set but not in the new set
void cleanChunks(const FilePath& cacheDir,
                 const json::Array &oldDefs, 
                 const json::Array &newDefs)
{
   Error error;
   std::vector<std::string> oldIds, newIds;

   // extract chunk IDs from JSON objects
   extractChunkIds(oldDefs, &oldIds);
   extractChunkIds(newDefs, &newIds);

   // compute the set of stale IDs
   std::vector<std::string> staleIds;
   std::sort(oldIds.begin(), oldIds.end());
   std::sort(newIds.begin(), newIds.end());
   std::set_difference(oldIds.begin(), oldIds.end(),
                       newIds.begin(), newIds.end(), 
                       std::back_inserter(staleIds));

   BOOST_FOREACH(const std::string& staleId, staleIds)
   {
      // clean chunk HTML and supporting files if present
      error = cacheDir.complete(staleId + ".html").removeIfExists();
      if (error)
         LOG_ERROR(error);
      error = cacheDir.complete(staleId + "_files").removeIfExists();
      if (error)
         LOG_ERROR(error);
      error = cacheDir.complete(staleId + ".csv").removeIfExists();
      if (error)
         LOG_ERROR(error);
   }
}

SEXP rs_populateNotebookCache(SEXP fileSEXP)
{
   std::string file = r::sexp::safeAsString(fileSEXP);
   FilePath cacheFolder = 
      chunkCacheFolder(file, "", userSettings().contextId());
   Error error = parseRnb(module_context::resolveAliasedPath(file), 
                          cacheFolder);
   if (error) 
      LOG_ERROR(error);

   r::sexp::Protect rProtect;
   return r::sexp::create(cacheFolder.absolutePath(), &rProtect);
}

} // anonymous namespace

Error setChunkDefs(const std::string& docPath, const std::string& docId,
                   std::time_t docTime, const json::Array& newDefs)
{
   // create JSON object wrapping 
   json::Object chunkDefs;
   chunkDefs[kChunkDefs] = newDefs;
   chunkDefs[kChunkDocWriteTime] = static_cast<boost::int64_t>(docTime);

   // ensure we have a place to write the sidecar file
   FilePath defFile = chunkDefinitionsPath(docPath, docId, 
         userSettings().contextId());

   // if there are no old chunk definitions and we aren't adding any new ones,
   // no work to do
   if (!defFile.exists() && newDefs.size() < 1) 
      return Success();

   // we're going to write something; make sure the parent folder exists
   Error error = ensureCacheFolder(defFile.parent());
   if (error)
      return error;

   // get the old set of chunk IDs so we can clean up any not in the new set 
   // of chunks
   std::vector<std::string> chunkIds;
   json::Value oldDefs;
   std::string oldContent;
   error = getChunkDefs(docPath, docId, userSettings().contextId(), NULL, 
         &oldDefs);
   if (error)
      LOG_ERROR(error);
   else if (oldDefs.type() == json::ArrayType)
   {
      if (oldDefs.get_array() == newDefs) 
      {
         // definitions not changing; no work to do
         return Success();
      }
      cleanChunks(chunkCacheFolder(docPath, docId),
                  oldDefs.get_array(), newDefs);
   }

   std::ostringstream oss;
   json::write(chunkDefs, oss);

   error = writeStringToFile(defFile, oss.str());
   if (error)
   {
      LOG_ERROR(error);
      return error;
   }
   
   return Success();
}

Error getChunkDefs(const std::string& docPath, const std::string& docId,
                   time_t *pDocTime, core::json::Value* pDefs)
{
   return getChunkDefs(docPath, docId, userSettings().contextId(), 
                       pDocTime, pDefs);
}


Error ensureCacheFolder(const FilePath& folder)
{
   Error error = folder.ensureDirectory();
   if (error)
      return error;
#ifdef _WIN32
   // on Windows, mark the directory hidden after creating it
   error = core::system::makeFileHidden(folder);
   if (error)
   {
      // non-fatal
      LOG_ERROR(error);
   }
#endif
   return error;
}

Error extractTagAttrs(const std::string& tag,
                      const std::string& attr,
                      const std::string& contents, 
                      std::vector<std::string>* pValues)
{
   std::string::const_iterator pos = contents.begin(); 

   // Not robust to all formulations (e.g. doesn't allow for attributes between
   // the tag and attr, or single-quoted/unquoted attributes), but we only need
   // to parse canonical Pandoc output
   boost::regex re("<\\s*" + tag + "\\s*" + attr + 
                   "\\s*=\\s*\"([^\"]+)\"[^>]*>", boost::regex::icase);

   // Iterate over all matches 
   boost::smatch match;
   while (boost::regex_search(pos, contents.end(), match, re, 
                              boost::match_default))
   {
      // record script src contents
      pValues->push_back(match.str(1));

      // continue search from end of match
      pos = match[0].second;
   }

   return Success();
}

Events& events()
{
   static Events instance;
   return instance;
}
Error initialize()
{
   using boost::bind;
   using namespace module_context;

   source_database::events().onDocRenamed.connect(onDocRenamed);
   source_database::events().onDocRemoved.connect(onDocRemoved);
   source_database::events().onDocAdded.connect(onDocAdded);

   module_context::events().onActiveConsoleChanged.connect(
         onActiveConsoleChanged);

   events().onChunkExecCompleted.connect(onChunkExecCompleted);

   R_CallMethodDef methodDef ;
   methodDef.name = "rs_populateNotebookCache";
   methodDef.fun = (DL_FUNC)rs_populateNotebookCache ;
   methodDef.numArgs = 1;
   r::routines::addCallMethod(methodDef);

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "execute_inline_chunk", executeInlineChunk))
      (bind(registerRpcMethod, "refresh_chunk_output", refreshChunkOutput))
      (bind(registerRpcMethod, "set_chunk_console", setChunkConsole))
      (bind(registerUriHandler, "/" kChunkOutputPath, 
            handleChunkOutputRequest))
      (bind(module_context::sourceModuleRFile, "SessionRmdNotebook.R"));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

