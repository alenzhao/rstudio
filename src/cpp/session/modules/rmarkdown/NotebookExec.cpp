/*
 * NotebookExec.cpp
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
#include "NotebookExec.hpp"
#include "NotebookOutput.hpp"
#include "NotebookPlots.hpp"
#include "NotebookHtmlWidgets.hpp"
#include "NotebookCache.hpp"

#include <core/text/CsvParser.hpp>
#include <core/FileSerializer.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/SessionUserSettings.hpp>

#include <iostream>

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

namespace {

bool moveLibFile(const FilePath& from, const FilePath& to, 
      const FilePath& path, std::vector<FilePath> *pPaths)
{
   std::string relativePath = path.relativePath(from);
   FilePath target = to.complete(relativePath);

   pPaths->push_back(path);

   Error error = path.isDirectory() ?
                     target.ensureDirectory() :
                     path.move(target);
   if (error)
      LOG_ERROR(error);
   return true;
}

} // anonymous namespace

ChunkExecContext::ChunkExecContext(const std::string& docId, 
      const std::string& chunkId):
   docId_(docId), 
   chunkId_(chunkId),
   consoleConnected_(false),
   plotsConnected_(false)
{
}

ChunkExecContext::~ChunkExecContext()
{
}

std::string ChunkExecContext::chunkId()
{
   return chunkId_;
}

std::string ChunkExecContext::docId()
{
   return docId_;
}

bool ChunkExecContext::consoleConnected()
{
   return consoleConnected_;
}

void ChunkExecContext::connect()
{
   FilePath outputPath = chunkOutputPath(docId_, chunkId_);
   Error error = outputPath.ensureDirectory();
   if (error)
   {
      // if we don't have a place to put the output, don't register any handlers
      // (will end in tears)
      LOG_ERROR(error);
      return;
   }

   // begin capturing console text
   module_context::events().onConsolePrompt.connect(
         boost::bind(&ChunkExecContext::onConsolePrompt, this, _1));
   module_context::events().onConsoleOutput.connect(
         boost::bind(&ChunkExecContext::onConsoleOutput, this, _1, _2));
   module_context::events().onConsoleInput.connect(
         boost::bind(&ChunkExecContext::onConsoleInput, this, _1));

   // begin capturing plots 
   events().onPlotOutput.connect(
         boost::bind(&ChunkExecContext::onFileOutput, this, _1, 
                     kChunkOutputPlot));
   events().onPlotOutputComplete.connect(
         boost::bind(&ChunkExecContext::onPlotOutputComplete, this));

   error = beginPlotCapture(outputPath);
   if (error)
      LOG_ERROR(error);
   else
      plotsConnected_ = true;

   // begin capturing HTML input
   events().onHtmlOutput.connect(
         boost::bind(&ChunkExecContext::onFileOutput, this, _1, 
                     kChunkOutputHtml));

   error = beginWidgetCapture(outputPath, 
         outputPath.parent().complete(kChunkLibDir));
   if (error)
      LOG_ERROR(error);

   consoleConnected_ = true;
}

void ChunkExecContext::onPlotOutputComplete()
{
   // disconnect from plot output events
   events().onPlotOutput.disconnect(
         boost::bind(&ChunkExecContext::onFileOutput, this, _1, kChunkOutputPlot));
   events().onPlotOutputComplete.disconnect(
         boost::bind(&ChunkExecContext::onPlotOutputComplete, this));
   plotsConnected_ = false;

   // if the console's not still connected, let the client know
   if (!consoleConnected_)
      events().onChunkExecCompleted(docId_, chunkId_, 
            userSettings().contextId());
}

void ChunkExecContext::onConsolePrompt(const std::string& )
{
   if (consoleConnected_)
      disconnect();
}

void ChunkExecContext::onFileOutput(const FilePath& file, int outputType)
{
   OutputPair pair = lastChunkOutput(docId_, chunkId_);
   pair.ordinal++;
   pair.outputType = outputType;
   FilePath target = chunkOutputFile(docId_, chunkId_, pair);
   Error error = file.move(target);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // check to see if the file has an accompanying library folder; if so, move
   // it to the global library folder
   FilePath fileLib = file.parent().complete(kChunkLibDir);
   if (fileLib.exists())
   {
      std::vector<FilePath> paths;
      error = fileLib.childrenRecursive(boost::bind(moveLibFile, fileLib,
            chunkCacheFolder(docId_, chunkId_)
                           .complete(kChunkLibDir), _2, &paths));
      if (error)
         LOG_ERROR(error);
      // TODO: Write this to JSON, too, for accounting
      error = fileLib.remove();
      if (error)
         LOG_ERROR(error);
   }
   
   enqueueChunkOutput(docId_, chunkId_, outputType, target);
   updateLastChunkOutput(docId_, chunkId_, pair);
}

void ChunkExecContext::onConsoleText(int type, const std::string& output, 
      bool truncate)
{
   if (output.empty())
      return;

   FilePath outputCsv = chunkOutputFile(docId_, chunkId_, kChunkOutputText);

   std::vector<std::string> vals; 
   vals.push_back(safe_convert::numberToString(type));
   vals.push_back(output);
   Error error = core::writeStringToFile(outputCsv, 
         text::encodeCsvLine(vals) + "\n", 
         string_utils::LineEndingPassthrough, truncate);
   if (error)
   {
      LOG_ERROR(error);
   }

   events().onChunkConsoleOutput(docId_, chunkId_, type, output);
}

void ChunkExecContext::disconnect()
{
   module_context::events().onConsolePrompt.disconnect(
         boost::bind(&ChunkExecContext::onConsolePrompt, this));
   module_context::events().onConsoleOutput.disconnect(
         boost::bind(&ChunkExecContext::onConsoleOutput, this));
   module_context::events().onConsoleInput.disconnect(
         boost::bind(&ChunkExecContext::onConsoleInput, this));
   
   // disconnect HTML output (plots may still need to accumulate async)
   events().onHtmlOutput.disconnect(
         boost::bind(&ChunkExecContext::onFileOutput, this, _1, 
                     kChunkOutputHtml));

   // if the plots are no longer connected (could happen in the case of error
   // or early termination) let the client know
   if (!plotsConnected_)
      events().onChunkExecCompleted(docId_, chunkId_, 
         userSettings().contextId());

   consoleConnected_ = false;
}

void ChunkExecContext::onConsoleOutput(module_context::ConsoleOutputType type, 
      const std::string& output)
{
   if (type == module_context::ConsoleOutputNormal)
      onConsoleText(kChunkConsoleOutput, output, false);
   else
      onConsoleText(kChunkConsoleError, output, false);
}

void ChunkExecContext::onConsoleInput(const std::string& input)
{
   onConsoleText(kChunkConsoleInput, input, false);
}


} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio
