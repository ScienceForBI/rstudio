#
# SessionRmdNotebook.R
#
# Copyright (C) 2009-16 by RStudio, Inc.
#
# Unless you have received this program directly from RStudio pursuant
# to the terms of a commercial license agreement with RStudio, then
# this program is licensed to you under the terms of version 3 of the
# GNU Affero General Public License. This program is distributed WITHOUT
# ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
# MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
# AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
#
#
assign(".rs.notebookVersion", envir = .rs.toolsEnv(), "1.0")

.rs.addJsonRpcHandler("extract_rmd_from_notebook", function(input, output)
{
  if (Encoding(input) == "unknown") Encoding(input) <- "UTF-8"
  if (Encoding(output) == "unknown") Encoding(output) <- "UTF-8"

   # if 'output' already exists, compare file write times to determine
   # whether we really want to overwrite a pre-existing .Rmd
   if (file.exists(output)) {
      inputInfo  <- file.info(input)
      outputInfo <- file.info(output)
      
      if (outputInfo$mtime > inputInfo$mtime)
         stop("'", output, "' exists and is newer than '", input, "'")
   }
   
   contents <- .rs.extractFromNotebook("rnb-document-source", input)
   cat(contents, file = output, sep = "\n")

   .rs.scalar(TRUE)
})

.rs.addFunction("extractFromNotebook", function(tag, rnbPath)
{
   if (!file.exists(rnbPath))
      stop("no file at path '", rnbPath, "'")
   
   contents <- readLines(rnbPath, warn = FALSE)
   
   # find the line hosting the encoded content
   marker <- paste('<!--', tag)
   idx <- NULL
   for (i in seq_along(contents))
   {
      if (.rs.startsWith(contents[[i]], marker))
      {
         idx <- i
         break
      }
   }
   
   if (!length(idx))
      stop("no encoded content with tag '", tag, "' in '", rnbPath, "'")
      
   reDocument <- paste('<!--', tag, '(\\S+) -->')
   rmdEncoded <- sub(reDocument, "\\1", contents[idx])
   caTools::base64decode(rmdEncoded, character())
})

.rs.addFunction("executeSingleChunk", function(options,
                                               content,
                                               libDir,
                                               headerFile,
                                               outputFile) 
{
  # presume paths are UTF-8 encoded unless specified otherwise
  if (Encoding(libDir) == "unknown") Encoding(libDir) <- "UTF-8"
  if (Encoding(headerFile) == "unknown") Encoding(headerFile) <- "UTF-8"
  if (Encoding(outputFile) == "unknown") Encoding(outputFile) <- "UTF-8"

  # create a temporary file stub to send to R Markdown
  chunkFile <- tempfile(fileext = ".Rmd")
  chunk <- paste(paste("```{r", options, "echo=FALSE}"),
                 content,
                 "```\n", 
                 sep = "\n");
  writeLines(chunk, con = chunkFile)
  on.exit(unlink(chunkFile), add = TRUE)
  
  # render chunks directly in .GlobalEnv
  # TODO: use .rs.getActiveFrame()? use sandbox env
  # that has .GlobalEnv as parent?
  envir <- .GlobalEnv
                 
  # begin capturing the error stream (and clean up when we're done)
  errorFile <- tempfile()
  on.exit(unlink(errorFile), add = TRUE)
  errorCon <- file(errorFile, open = "wt")
  sink(errorCon, type = "message")
  on.exit(sink(type = "message"), add = TRUE)
  on.exit(close(errorCon), add = TRUE)

  # render the stub to the given file
  errorMessage <- ""
  errorText <- ""
  tryCatch({
    capture.output(rmarkdown::render(
      input = normalizePath(chunkFile, winslash = "/"), 
      output_format = rmarkdown::html_document(
        theme = NULL,
        highlight = NULL,
        template = NULL, 
        self_contained = FALSE,
        includes = list(
          in_header = headerFile),
        lib_dir = libDir),
      output_file = normalizePath(outputFile, winslash = "/", mustWork = FALSE),
      encoding = "UTF-8",
      envir = envir,
      quiet = TRUE))
  }, error = function(e) {
    # capture any error message returned
    errorMessage <<- paste("Error:", e$message)

    # flush the error stream and send it as well
    errorText <<- paste(readLines(errorFile), collapse = "\n")
  })

  list(message = errorMessage, 
       text    = errorText)
})

.rs.addFunction("injectHTMLComments", function(contents,
                                               location,
                                               inject)
{
   # find the injection location
   idx <- NULL
   for (i in seq_along(contents))
   {
      if (contents[[i]] == location)
      {
         idx <- i
         break
      }
   }
   
   if (is.null(idx))
      stop("failed to find injection location '", location, "'")
   
   # generate injection strings
   injection <- paste(vapply(seq_along(inject), FUN.VALUE = character(1), function(i) {
      sprintf('<!-- %s %s -->', names(inject)[i], inject[[i]])
   }), collapse = "\n")
   
   contents[[idx]] <- paste(contents[[idx]], injection, "", sep = "\n")
   contents
})

.rs.addFunction("createNotebook", function(inputFile,
                                           outputFile = NULL,
                                           envir = .GlobalEnv)
{
   find_chunks <- function(contents) {
      chunkStarts <- grep("^\\s*```{", contents, perl = TRUE)
      chunkEnds <- grep("^\\s*```\\s*$", contents, perl = TRUE)
      chunkRanges <- Map(list, start = chunkStarts, end = chunkEnds)
      lapply(chunkRanges, function(range) {
         list(start = range$start,
              end = range$end,
              header = contents[range$start],
              contents = contents[(range$start + 1):(range$end - 1)])
      })
   }
   
   chunk_annotation <- function(name, index, row) {
      sprintf("\n\n<!-- rnb-chunk-%s-%s %s -->\n\n", name, index, row)
   }
   
   # resolve input, output paths
   inputFile <- normalizePath(inputFile, winslash = "/", mustWork = TRUE)
   if (is.null(outputFile))
      outputFile <- .rs.withChangedExtension(inputFile, "Rnb")
   
   rmdContents <- readLines(inputFile, warn = FALSE)
   
   # inject placeholders so we can track chunk locations after render
   # ensure that the comments are surrounded by newlines, as otherwise
   # strange render errors can occur
   rmdModified <- rmdContents
   rmdChunks <- find_chunks(rmdModified)
   for (i in seq_along(rmdChunks)) {
      startIdx <- rmdChunks[[i]]$start
      rmdModified[startIdx] <- paste(
         chunk_annotation("start", i, startIdx),
         rmdModified[startIdx],
         sep = ""
      )
      
      endIdx <- rmdChunks[[i]]$end
      rmdModified[endIdx] <- paste(
         rmdModified[endIdx],
         chunk_annotation("end", i, endIdx),
         sep = ""
      )
   }
   
   # write out file and prepare for render
   renderInput  <- tempfile("rnb-render-input-", fileext = ".Rmd")
   writeLines(rmdModified, renderInput)
   on.exit(unlink(renderInput), add = TRUE)
   
   # perform render
   .rs.rnb.render(inputFile = renderInput,
                  outputFile = outputFile,
                  rmdContents = rmdContents)
   
   invisible(outputFile)
   
})

.rs.addFunction("rnb.withChunkLocations", function(rmdContents, chunkInfo)
{
   chunkLocs <- grep("^\\s*```{", rmdContents, perl = TRUE)
   for (i in seq_along(chunkInfo$chunk_definitions)) {
      info <- chunkInfo$chunk_definitions[[i]]
      
      info$chunk_start <- tail(chunkLocs[chunkLocs < info$row + 1], 1)
      info$chunk_end   <- info$row + 1
      
      chunkInfo$chunk_definitions[[i]] <- info
      
   }
   names(chunkInfo$chunk_definitions) <-
      unlist(lapply(chunkInfo$chunk_definitions, "[[", "chunk_id"))
   chunkInfo
})

.rs.addFunction("readRnbCache", function(rmdPath, cachePath)
{
   if (Encoding(rmdPath) == "unknown")   Encoding(rmdPath) <- "UTF-8"
   if (Encoding(cachePath) == "unknown") Encoding(cachePath) <- "UTF-8"
   
   if (!file.exists(rmdPath))
      stop("No file at path '", rmdPath, "'")
   
   if (!file.exists(cachePath))
      stop("No cache directory at path '", cachePath, "'")
   
   rmdPath <- .rs.normalizePath(rmdPath, winslash = "/", mustWork = TRUE)
   cachePath <- normalizePath(cachePath, winslash = "/", mustWork = TRUE)
   rmdContents <- suppressWarnings(readLines(rmdPath))
   
   # Begin collecting the units that form the Rnb data structure
   rnbData <- list()
   
   # store reference to source path
   rnbData[["source_path"]] <- rmdPath
   rnbData[["cache_path"]]  <- cachePath
   
   # Keep the original source data
   rnbData[["contents"]] <- rmdContents
   
   # Read the chunk information
   chunkInfoPath <- file.path(cachePath, "chunks.json")
   chunkInfo <- .rs.fromJSON(.rs.readFile(chunkInfoPath))
   
   # Augment with start, end locations of chunks
   chunkInfo <- .rs.rnb.withChunkLocations(rmdContents, chunkInfo)
   rnbData[["chunk_info"]] <- chunkInfo
   
   # Collect all of the HTML files, alongside their dependencies
   htmlFiles <- list.files(cachePath, pattern = "html$", full.names = TRUE)
   chunkData <- lapply(htmlFiles, function(file) {
      dependenciesDir <- paste(tools::file_path_sans_ext(file), "files", sep = "_")
      dependenciesFiles <- list.files(dependenciesDir, full.names = TRUE, recursive = TRUE)
      list(
         html = .rs.readFile(file),
         deps = lapply(dependenciesFiles, .rs.readFile)
      )
   })
   names(chunkData) <- tools::file_path_sans_ext(basename(htmlFiles))
   rnbData[["chunk_data"]] <- chunkData
   
   # Read in the 'libs' directory.
   rnbData[["lib"]] <- list()
   
   libDir <- file.path(cachePath, "lib")
   if (file.exists(libDir)) {
      owd <- setwd(libDir)
      libFiles <- list.files(libDir, recursive = TRUE)
      libData <- lapply(libFiles, .rs.readFile)
      names(libData) <- libFiles
      rnbData[["lib"]] <- libData
      setwd(owd)
   }
   
   rnbData
})

.rs.addFunction("extractHTMLBodyElement", function(html)
{
   begin <- regexpr('<body[^>]*>', html, perl = TRUE)
   end   <- regexpr('</body>', html, perl = TRUE)
   
   contents <- substring(html, begin + attr(begin, "match.length"), end - 1)
   .rs.trimWhitespace(contents)
})

.rs.addFunction("rnb.injectBase64Data", function(html, chunkId, rnbData)
{
   # we'll be calling pandoc in the cache dir, so make sure
   # we save our original dir
   owd <- getwd()
   on.exit(setwd(owd), add = TRUE)
   setwd(rnbData$cache_path)
   
   # use pandoc to render the HTML fragment, thereby injecting
   # base64-encoded dependencies
   input  <- "rnb-base64-inject-input.html"
   output <- "rnb-base64-inject-output.html"
   
   cat(html, file = input, sep = "\n")
   opts <- c("--self-contained")
   rmarkdown:::pandoc_convert(input, output = output, options = opts)
   
   result <- .rs.readFile(output)
   .rs.extractHTMLBodyElement(result)
})

.rs.addFunction("rnb.maskChunks", function(contents, chunkInfo)
{
   masked <- contents
   
   # Extract chunk locations based on the document + chunk info
   chunkRanges <- lapply(chunkInfo$chunk_definitions, function(info) {
      list(start = info$chunk_start,
           end   = info$chunk_end,
           id    = info$chunk_id)
   })
   
   for (range in rev(chunkRanges)) {
      masked <- c(
         masked[1:(range$start - 1)],
         paste("<!-- rnb-chunk-id", range$id, "-->"),
         masked[(range$end + 1):length(masked)]
      )
   }
   
   masked
   
})

.rs.addFunction("rnb.fillChunks", function(html, rnbData)
{
   filled <- html
   chunkIdx <- 1
   for (i in seq_along(filled)) {
      line <- filled[[i]]
      if (!(.rs.startsWith(line, "<!-- rnb-chunk-id") && .rs.endsWith(line, "-->")))
         next
      
      chunkId <- sub('<!-- rnb-chunk-id\\s*(\\S+)\\s*-->', '\\1', line)
      chunkData <- rnbData$chunk_data[[chunkId]]
      chunkDefn <- rnbData$chunk_info$chunk_definitions[[chunkId]]
      if (is.null(chunkData) || is.null(chunkDefn))
         stop("no chunk with id '", chunkId, "'")
      
      # inject PNGs etc. as base64 data
      injected <- .rs.rnb.injectBase64Data(chunkData$html, chunkId, rnbData)
      
      # insert into document
      injection <- c(
         sprintf("<!-- rnb-chunk-start-%s %s -->", chunkIdx, chunkDefn$chunk_start),
         paste(injected, collapse = "\n"),
         sprintf("<!-- rnb-chunk-end-%s %s -->", chunkIdx, chunkDefn$chunk_end)
      )
      
      filled[[i]] <- paste(injection, sep = "\n", collapse = "\n")
      chunkIdx <- chunkIdx + 1
   }
   filled
})

.rs.addFunction("rnb.render", function(inputFile,
                                       outputFile,
                                       rmdContents = .rs.readFile(inputFile),
                                       envir = .GlobalEnv)
{
   renderOutput <- tempfile("rnb-render-output-", fileext = ".html")
   outputFormat <- rmarkdown::html_document(self_contained = TRUE,
                                            keep_md = TRUE)
   
   rmarkdown::render(input = inputFile,
                     output_format = outputFormat,
                     output_file = renderOutput,
                     output_options = list(self_contained = TRUE),
                     encoding = "UTF-8",
                     envir = envir,
                     quiet = TRUE)
   
   
   # read the rendered file
   rnbContents <- readLines(renderOutput, warn = FALSE)
   
   # generate base64-encoded versions of .Rmd source, .md sidecar
   rmdEncoded <- caTools::base64encode(paste(rmdContents, collapse = "\n"))
   
   # inject document contents into rendered file
   # (i heard you like documents, so i put a document in your document)
   rnbContents <- .rs.injectHTMLComments(
      rnbContents,
      "<head>",
      list("rnb-document-source" = rmdEncoded)
   )
   
   # write our .Rnb to file and we're done!
   cat(rnbContents, file = outputFile, sep = "\n")
   invisible(outputFile)
   
})

.rs.addFunction("createNotebookFromCacheData", function(rnbData,
                                                        outputFile,
                                                        envir = .GlobalEnv)
{
   # first, render our .Rmd to transform markdown to html
   contents <- rnbData$contents
   chunkInfo <- rnbData$chunk_info
   
   # mask out chunks (replace with placeholders w/id)
   masked <- .rs.rnb.maskChunks(contents, chunkInfo)
   
   # use pandoc to convert md to html
   inputTemp  <- tempfile("rnb-tempfile-input-", fileext = ".md")
   outputTemp <- tempfile("rnb-tempfile-output-", fileext = ".html")
   cat(masked, file = inputTemp, sep = "\n")
   
   # render our notebook
   .rs.rnb.render(inputFile = inputTemp,
                  outputFile = outputTemp,
                  rmdContents = contents,
                  envir = envir)
   
   # read the HTML
   html <- readLines(outputTemp)
   
   # replace chunk placeholders with their actual data
   html <- .rs.rnb.fillChunks(html, rnbData)
   
   # write to file
   cat(html, file = outputFile, sep = "\n")
   outputFile
})
