/*
 * NotebookPlots.cpp
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
#include "NotebookPlots.hpp"

#include <boost/format.hpp>
#include <boost/foreach.hpp>

#include <core/system/FileMonitor.hpp>
#include <core/StringUtils.hpp>

#include <r/RExec.hpp>

#include <session/SessionModuleContext.hpp>

#define kPlotPrefix "_rs_chunk_plot_"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {
namespace {

bool isPlotPath(const FilePath& path)
{
   return path.hasExtensionLowerCase(".png") &&
          string_utils::isPrefixOf(path.stem(), kPlotPrefix);
}

bool plotFilter(const FileInfo& file)
{
   return file.isDirectory() || isPlotPath(FilePath(file.absolutePath()));
}

void removeGraphicsDevice(const FilePath& plotFolder)
{
   // turn off the graphics device -- this has the side effect of writing the
   // device's remaining output to files
   Error error = r::exec::RFunction("dev.off").call();
   if (error)
      LOG_ERROR(error);

   // clean up any stale plots from the folder
   std::vector<FilePath> folderContents;
   error = plotFolder.children(&folderContents);
   if (error)
      LOG_ERROR(error);

   BOOST_FOREACH(const FilePath& path, folderContents)
   {
      if (isPlotPath(path))
         events().onPlotOutput(path);
   }

   events().onPlotOutputComplete();
}

void unregisterMonitor(const std::string&,
                       const core::system::file_monitor::Handle& handle)
{
   module_context::events().onConsolePrompt.disconnect(
         boost::bind(unregisterMonitor, _1, handle));

   core::system::file_monitor::unregisterMonitor(handle);
}

void onMonitorRegistered(
      const core::system::file_monitor::Handle& handle,
      const tree<FileInfo>& files)
{
   // we only want to listen until the next console prompt
   module_context::events().onConsolePrompt.connect(
         boost::bind(unregisterMonitor, _1, handle));

   // fire for any plots which were emitted during file monitor registration
   for (tree<FileInfo>::iterator it = files.begin(); it != files.end(); it++) 
   {
      FilePath path(it->absolutePath());
      if (isPlotPath(path))
         events().onPlotOutput(path);
   }
}

void onMonitorRegError(const FilePath& plotFolder, 
                       const Error &error)
{
   LOG_ERROR(error);
   removeGraphicsDevice(plotFolder);
}

void onMonitorUnregistered(const FilePath& plotFolder,
                           const core::system::file_monitor::Handle& handle)
{
   // when the monitor is unregistered, disable the associated graphics
   // device
   removeGraphicsDevice(plotFolder);
}

void onPlotFilesChanged(
      const std::vector<core::system::FileChangeEvent>& changeEvents)
{
   BOOST_FOREACH(const core::system::FileChangeEvent& event, changeEvents)
   {
      // we only care about new plots
      if (event.type() != core::system::FileChangeEvent::FileAdded)
         continue;

      // notify caller
      FilePath path(event.fileInfo().absolutePath());
      events().onPlotOutput(path);
   }
}

} // anonymous namespace

// begins capturing plot output
core::Error beginPlotCapture(const FilePath& plotFolder)
{
   // clean up any stale plots from the folder
   std::vector<FilePath> folderContents;
   Error error = plotFolder.children(&folderContents);
   if (error)
      return error;

   BOOST_FOREACH(const core::FilePath& file, folderContents)
   {
      // remove if it looks like a plot 
      if (isPlotPath(file)) 
      {
         error = file.remove();
         if (error)
         {
            // this is non-fatal 
            LOG_ERROR(error);
         }
      }
   }
   
   // generate code for creating PNG device
   boost::format fmt("{ require(grDevices, quietly=TRUE); "
                     "  png(file = \"%1%/" kPlotPrefix "%%03d.png\", "
                     "  width = 7, height = 7, "
                     "  units=\"in\", res = 96, type = \"cairo-png\", TRUE)"
                     "}");

   // create the PNG device
   error = r::exec::executeString(
         (fmt % plotFolder.absolutePath()).str());
   if (error)
      return error;

   // set up file monitor callbacks
   core::system::file_monitor::Callbacks callbacks;
   callbacks.onRegistered = onMonitorRegistered;
   callbacks.onUnregistered = boost::bind(onMonitorUnregistered,
         plotFolder, _1);
   callbacks.onRegistrationError = boost::bind(onMonitorRegError,
         plotFolder, _1);
   callbacks.onFilesChanged = onPlotFilesChanged;

   // create the monitor
   core::system::file_monitor::registerMonitor(plotFolder, true, plotFilter,
         callbacks);

   return Success();
}


} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio
