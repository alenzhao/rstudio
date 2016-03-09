/*
 * NotebookExec.hpp
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

#ifndef SESSION_NOTEBOOK_EXEC_HPP
#define SESSION_NOTEBOOK_EXEC_HPP

#include <session/SessionModuleContext.hpp>

namespace rstudio {
namespace core {
   class Error;
   class FilePath;
}
}
 
namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

class ChunkExecContext
{
public:
   // initialize a new execution context
   ChunkExecContext(const std::string& docId, const std::string& chunkId);
   ~ChunkExecContext();

   // connect or disconnect the execution context to events
   void connect();
   void disconnect();

   // return execution context from events
   std::string chunkId();
   std::string docId();
   bool consoleConnected();

private:
   void onConsoleOutput(module_context::ConsoleOutputType type, 
         const std::string& output);
   void onConsoleInput(const std::string& input);
   void onConsolePrompt(const std::string&);
   void onConsoleText(int type, const std::string& output, bool truncate);
   void onFileOutput(const core::FilePath& file, int outputType);
   void onPlotOutputComplete();

   std::string docId_;
   std::string chunkId_;
   bool consoleConnected_;
   bool plotsConnected_;
};

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

#endif // SESSION_NOTEBOOK_EXEC_HPP