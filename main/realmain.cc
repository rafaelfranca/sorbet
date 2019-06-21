// has to be included first as it violates our poisons
#include "core/proto/proto.h"

#include "absl/debugging/symbolize.h"
#include "absl/strings/str_cat.h"
#include "common/FileOps.h"
#include "common/Timer.h"
#include "common/statsd/statsd.h"
#include "common/web_tracer_framework/tracing.h"
#include "core/Error.h"
#include "core/Files.h"
#include "core/Unfreeze.h"
#include "core/errors/errors.h"
#include "core/lsp/QueryResponse.h"
#include "core/serialize/serialize.h"
#include "main/autogen/autogen.h"
#include "main/lsp/lsp.h"
#include "main/pipeline/pipeline.h"
#include "main/realmain.h"
#include "payload/payload.h"
#include "resolver/resolver.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "version/version.h"

#include <csignal>
#include <poll.h>

namespace spd = spdlog;

using namespace std;

namespace sorbet::realmain {
shared_ptr<spd::logger> logger;
int returnCode;

shared_ptr<spd::sinks::ansicolor_stderr_sink_mt> make_stderrColorSink() {
    auto color_sink = make_shared<spd::sinks::ansicolor_stderr_sink_mt>();
    color_sink->set_color(spd::level::info, color_sink->white);
    color_sink->set_color(spd::level::debug, color_sink->magenta);
    color_sink->set_level(spd::level::info);
    return color_sink;
}

shared_ptr<spd::sinks::ansicolor_stderr_sink_mt> stderrColorSink = make_stderrColorSink();

/*
 * Workaround https://bugzilla.mindrot.org/show_bug.cgi?id=2863 ; We are
 * commonly run under ssh with a controlmaster, and we write exclusively to
 * STDERR in normal usage. If the client goes away, we can hang forever writing
 * to a full pipe buffer on stderr.
 *
 * Workaround by monitoring for STDOUT to go away and self-HUPing.
 */
void startHUPMonitor() {
    thread monitor([]() {
        struct pollfd pfd;
        setCurrentThreadName("HUPMonitor");
        pfd.fd = 1; // STDOUT
        pfd.events = 0;
        pfd.revents = 0;
        while (true) {
            int rv = poll(&pfd, 1, -1);
            if (rv <= 0) {
                continue;
            }
            if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
                // STDOUT has gone away; Exit via SIGHUP.
                kill(getpid(), SIGHUP);
            }
        }
    });
    monitor.detach();
}

void addStandardMetrics() {
    prodCounterAdd("release.build_scm_commit_count", Version::build_scm_commit_count);
    prodCounterAdd("release.build_timestamp",
                   chrono::duration_cast<std::chrono::seconds>(Version::build_timestamp.time_since_epoch()).count());
    StatsD::addRusageStats();
}

core::StrictLevel levelMinusOne(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::Ignore:
            return core::StrictLevel::None;
        case core::StrictLevel::False:
            return core::StrictLevel::Ignore;
        case core::StrictLevel::True:
            return core::StrictLevel::False;
        case core::StrictLevel::Strict:
            return core::StrictLevel::True;
        case core::StrictLevel::Strong:
            return core::StrictLevel::Strict;
        case core::StrictLevel::Max:
            return core::StrictLevel::Strong;
        default:
            Exception::raise("Should never happen");
    }
}

string levelToSigil(core::StrictLevel level) {
    switch (level) {
        case core::StrictLevel::None:
            Exception::raise("Should never happen");
        case core::StrictLevel::Internal:
            Exception::raise("Should never happen");
        case core::StrictLevel::Ignore:
            return "ignore";
        case core::StrictLevel::False:
            return "false";
        case core::StrictLevel::True:
            return "true";
        case core::StrictLevel::Strict:
            return "strict";
        case core::StrictLevel::Strong:
            return "strong";
        case core::StrictLevel::Max:
            Exception::raise("Should never happen");
        case core::StrictLevel::Autogenerated:
            Exception::raise("Should never happen");
        case core::StrictLevel::Stdlib:
            return "__STDLIB_INTERNAL";
    }
}

core::Loc findTyped(unique_ptr<core::GlobalState> &gs, core::FileRef file) {
    auto source = file.data(*gs).source();

    if (file.data(*gs).originalSigil == core::StrictLevel::None) {
        if (source.length() >= 2 && source[0] == '#' && source[1] == '!') {
            int newline = source.find("\n", 0);
            return core::Loc(file, newline + 1, newline + 1);
        }
        return core::Loc(file, 0, 0);
    }
    size_t start = 0;
    start = source.find("typed:", start);
    if (start == string_view::npos) {
        return core::Loc(file, 0, 0);
    }
    while (start >= 0 && source[start] != '#') {
        --start;
    }
    auto end = start;
    while (end < source.size() && source[end] != '\n') {
        ++end;
    }
    if (source[end] == '\n') {
        ++end;
    }
    return core::Loc(file, start, end);
}

struct AutogenResult {
    struct Serialized {
        // Selectively populated based on print options
        string strval;
        string msgpack;
        vector<string> classlist;
        map<string, set<string>> subclasses;
    };
    CounterState counters;
    vector<pair<int, Serialized>> prints;
};

void runAutogen(core::Context ctx, options::Options &opts, WorkerPool &workers, vector<ast::ParsedFile> &indexed) {
    Timer timeit(logger, "autogen");

    auto resultq = make_shared<BlockingBoundedQueue<AutogenResult>>(indexed.size());
    auto fileq = make_shared<ConcurrentBoundedQueue<int>>(indexed.size());
    for (int i = 0; i < indexed.size(); ++i) {
        fileq->push(move(i), 1);
    }

    workers.multiplexJob("runAutogen", [&ctx, &opts, &indexed, fileq, resultq]() {
        AutogenResult out;
        int n = 0;
        {
            Timer timeit(logger, "autogenWorker");
            int idx = 0;

            for (auto result = fileq->try_pop(idx); !result.done(); result = fileq->try_pop(idx)) {
                ++n;
                auto &tree = indexed[idx];
                if (tree.file.data(ctx).isRBI()) {
                    continue;
                }
                auto pf = autogen::Autogen::generate(ctx, move(tree));
                tree = move(pf.tree);

                AutogenResult::Serialized serialized;
                if (opts.print.Autogen.enabled) {
                    Timer timeit(logger, "autogenToString");
                    serialized.strval = pf.toString(ctx);
                }
                if (opts.print.AutogenMsgPack.enabled) {
                    Timer timeit(logger, "autogenToMsgpack");
                    serialized.msgpack = pf.toMsgpack(ctx, opts.autogenVersion);
                }
                if (opts.print.AutogenClasslist.enabled) {
                    Timer timeit(logger, "autogenClasslist");
                    pf.classlist(ctx, serialized.classlist);
                }
                if (opts.print.AutogenSubclasses.enabled) {
                    Timer timeit(logger, "autogenSubclasses");
                    pf.subclasses(ctx, opts.autogenSubclassesParents, opts.autogenSubclassesAbsoluteIgnorePatterns,
                                  opts.autogenSubclassesRelativeIgnorePatterns, serialized.subclasses);
                }
                out.prints.emplace_back(make_pair(idx, serialized));
            }
        }

        out.counters = getAndClearThreadCounters();
        resultq->push(move(out), n);
    });

    AutogenResult out;
    vector<pair<int, AutogenResult::Serialized>> merged;
    for (auto res = resultq->wait_pop_timed(out, chrono::seconds{1}, *logger); !res.done();
         res = resultq->wait_pop_timed(out, chrono::seconds{1}, *logger)) {
        if (!res.gotItem()) {
            continue;
        }
        counterConsume(move(out.counters));
        merged.insert(merged.end(), make_move_iterator(out.prints.begin()), make_move_iterator(out.prints.end()));
    }
    fast_sort(merged, [](const auto &lhs, const auto &rhs) -> bool { return lhs.first < rhs.first; });

    for (auto &elem : merged) {
        if (opts.print.Autogen.enabled) {
            opts.print.Autogen.print(elem.second.strval);
        }
        if (opts.print.AutogenMsgPack.enabled) {
            opts.print.AutogenMsgPack.print(elem.second.msgpack);
        }
    }
    if (opts.print.AutogenClasslist.enabled) {
        Timer timeit(logger, "autogenClasslistPrint");
        vector<string> mergedClasslist;
        for (auto &el : merged) {
            auto &v = el.second.classlist;
            mergedClasslist.insert(mergedClasslist.end(), make_move_iterator(v.begin()), make_move_iterator(v.end()));
        }
        fast_sort(mergedClasslist);
        auto last = unique(mergedClasslist.begin(), mergedClasslist.end());
        opts.print.AutogenClasslist.fmt("{}\n", fmt::join(mergedClasslist.begin(), last, "\n"));
    }
    if (opts.print.AutogenSubclasses.enabled) {
        Timer timeit(logger, "autogenSubclassesPrint");

        // Merge the {Parent: Set{Child1, Child2}} maps from each thread
        map<string, set<string>> mergedSubclasses;
        for (auto &el : merged) {
            for (auto const &[parentName, children] : el.second.subclasses) {
                if (parentName.empty()) {
                    // Child < NonexistentParent
                    continue;
                }
                mergedSubclasses[parentName].insert(children.begin(), children.end());
            }
        }

        vector<string> mergedSubclasseslist;
        for (auto const &[parentName, children] : mergedSubclasses) {
            mergedSubclasseslist.insert(mergedSubclasseslist.end(), parentName);
            for (auto const child : children) {
                mergedSubclasseslist.insert(mergedSubclasseslist.end(), fmt::format(" {}", child));
            }
        }
        // TODO(gwu) manual_patch
        // TODO(gwu) descendants_of for each class passed in at the CLI
        // port descendants_of(klass, out) and call in a loop
        opts.print.AutogenSubclasses.fmt("{}\n",
                                         fmt::join(mergedSubclasseslist.begin(), mergedSubclasseslist.end(), "\n"));
    }
} // namespace sorbet::realmain

int realmain(int argc, char *argv[]) {
    absl::InitializeSymbolizer(argv[0]);
    returnCode = 0;
    logger = make_shared<spd::logger>("console", stderrColorSink);
    logger->set_level(spd::level::trace); // pass through everything, let the sinks decide
    logger->set_pattern("%v");
    fatalLogger = logger;

    auto typeErrorsConsole = make_shared<spd::logger>("typeDiagnostics", stderrColorSink);
    typeErrorsConsole->set_pattern("%v");

    options::Options opts;
    options::readOptions(opts, argc, argv, logger);
    while (opts.waitForDebugger && !stopInDebugger()) {
        // spin
    }
    if (opts.stdoutHUPHack) {
        startHUPMonitor();
    }
    if (!opts.debugLogFile.empty()) {
        // LSP could run for a long time. Rotate log files, and trim at 1 GiB. Keep around 3 log files.
        // Cast first number to size_t to prevent integer multiplication.
        // TODO(jvilk): Reduce size once LSP logging is less chunderous.
        auto fileSink =
            make_shared<spdlog::sinks::rotating_file_sink_mt>(opts.debugLogFile, ((size_t)1) * 1024 * 1024 * 1024, 3);
        fileSink->set_level(spd::level::debug);
        { // replace console & fatal loggers
            vector<spd::sink_ptr> sinks{stderrColorSink, fileSink};
            auto combinedLogger = make_shared<spd::logger>("consoleAndFile", begin(sinks), end(sinks));
            combinedLogger->flush_on(spdlog::level::err);
            combinedLogger->set_level(spd::level::trace); // pass through everything, let the sinks decide

            spd::register_logger(combinedLogger);
            fatalLogger = combinedLogger;
            logger = combinedLogger;
        }
        { // replace type error logger
            vector<spd::sink_ptr> sinks{stderrColorSink, fileSink};
            auto combinedLogger = make_shared<spd::logger>("typeDiagnosticsAndFile", begin(sinks), end(sinks));
            spd::register_logger(combinedLogger);
            combinedLogger->set_level(spd::level::trace); // pass through everything, let the sinks decide
            typeErrorsConsole = combinedLogger;
        }
    }
    // Use a custom formatter so we don't get a default newline

    switch (opts.logLevel) {
        case 0:
            stderrColorSink->set_level(spd::level::info);
            break;
        case 1:
            stderrColorSink->set_level(spd::level::debug);
            logger->set_pattern("[T%t][%Y-%m-%dT%T.%f] %v");
            logger->debug("Debug logging enabled");
            break;
        default:
            stderrColorSink->set_level(spd::level::trace);
            logger->set_pattern("[T%t][%Y-%m-%dT%T.%f] %v");
            logger->trace("Trace logging enabled");
            break;
    }

    {
        string argsConcat(argv[0]);
        for (int i = 1; i < argc; i++) {
            absl::StrAppend(&argsConcat, " ", argv[i]);
        }
        logger->debug("Running sorbet version {} with arguments: {}", Version::full_version_string, argsConcat);
        if (!Version::isReleaseBuild && !opts.silenceDevMessage &&
            std::getenv("SORBET_SILENCE_DEV_MESSAGE") == nullptr) {
            logger->info("👋 Hey there! Heads up that this is not a release build of sorbet.\n"
                         "Release builds are faster and more well-supported by the Sorbet team.\n"
                         "Check out the README to learn how to build Sorbet in release mode.\n"
                         "To forcibly silence this error, either pass --silence-dev-message,\n"
                         "or set SORBET_SILENCE_DEV_MESSAGE=1 in your shell environment.\n");
        }
    }
    unique_ptr<WorkerPool> workers = WorkerPool::create(opts.threads, *logger);

    unique_ptr<core::GlobalState> gs =
        make_unique<core::GlobalState>((make_shared<core::ErrorQueue>(*typeErrorsConsole, *logger)));
    gs->pathPrefix = opts.pathPrefix;
    gs->errorUrlBase = opts.errorUrlBase;
    vector<ast::ParsedFile> indexed;

    logger->trace("building initial global state");
    unique_ptr<KeyValueStore> kvstore;
    if (!opts.cacheDir.empty()) {
        kvstore = make_unique<KeyValueStore>(Version::full_version_string, opts.cacheDir,
                                             opts.skipDSLPasses ? "nodsl" : "default");
    }
    payload::createInitialGlobalState(gs, opts, kvstore);
    if (opts.silenceErrors) {
        gs->silenceErrors = true;
    }
    if (opts.autocorrect) {
        gs->autocorrect = true;
    }
    if (opts.suggestRuntimeProfiledType) {
        gs->suggestRuntimeProfiledType = true;
    }
    if (opts.print.Autogen.enabled || opts.print.AutogenMsgPack.enabled || opts.print.AutogenClasslist.enabled ||
        opts.print.AutogenSubclasses.enabled) {
        gs->runningUnderAutogen = true;
    }
    if (opts.reserveMemKiB > 0) {
        gs->reserveMemory(opts.reserveMemKiB);
    }
    for (auto code : opts.errorCodeWhiteList) {
        gs->onlyShowErrorClass(code);
    }
    for (auto code : opts.errorCodeBlackList) {
        gs->suppressErrorClass(code);
    }
    for (auto &plugin : opts.dslPluginTriggers) {
        core::UnfreezeNameTable nameTableAccess(*gs);
        gs->addDslPlugin(plugin.first, plugin.second);
    }
    gs->dslRubyExtraArgs = opts.dslRubyExtraArgs;

    logger->trace("done building initial global state");

    if (opts.runLSP) {
        gs->errorQueue->ignoreFlushes = true;
        logger->debug("Starting sorbet version {} in LSP server mode. "
                      "Talk ‘\\r\\n’-separated JSON-RPC to me. "
                      "More details at https://microsoft.github.io/language-server-protocol/specification."
                      "If you're developing an LSP extension to some editor, make sure to run sorbet with `-v` flag,"
                      "it will enable outputing the LSP session to stderr(`Write: ` and `Read: ` log lines)",
                      Version::full_version_string);
        lsp::LSPLoop loop(move(gs), opts, logger, *workers, STDIN_FILENO, cout);
        gs = loop.runLSP();
    } else {
        Timer timeall(logger, "wall_time");
        vector<core::FileRef> inputFiles;
        logger->trace("Files: ");

        { inputFiles = pipeline::reserveFiles(gs, opts.inputFileNames); }

        {
            core::UnfreezeFileTable fileTableAccess(*gs);
            if (!opts.inlineInput.empty()) {
                prodCounterAdd("types.input.bytes", opts.inlineInput.size());
                prodCounterInc("types.input.lines");
                prodCounterInc("types.input.files");
                auto input = opts.inlineInput;
                if (core::File::fileSigil(opts.inlineInput) == core::StrictLevel::None) {
                    // put it at the end so as to not upset line numbers
                    input += "\n# typed: true";
                }
                auto file = gs->enterFile(string("-e"), input);
                inputFiles.emplace_back(file);
            }
        }

        { indexed = pipeline::index(gs, inputFiles, opts, *workers, kvstore); }

        payload::retainGlobalState(gs, opts, kvstore);

        if (gs->runningUnderAutogen) {
            gs->suppressErrorClass(core::errors::Namer::MethodNotFound.code);
            gs->suppressErrorClass(core::errors::Namer::RedefinitionOfMethod.code);
            gs->suppressErrorClass(core::errors::Namer::ModuleKindRedefinition.code);
            gs->suppressErrorClass(core::errors::Resolver::StubConstant.code);

            core::MutableContext ctx(*gs, core::Symbols::root());

            indexed = pipeline::name(*gs, move(indexed), opts);
            {
                core::UnfreezeNameTable nameTableAccess(*gs);
                core::UnfreezeSymbolTable symbolAccess(*gs);

                vector<core::ErrorRegion> errs;
                for (auto &tree : indexed) {
                    auto file = tree.file;
                    errs.emplace_back(*gs, file);
                }
                indexed = resolver::Resolver::runConstantResolution(ctx, move(indexed), *workers);
            }

            runAutogen(ctx, opts, *workers, indexed);
        } else {
            indexed = pipeline::resolve(gs, move(indexed), opts, *workers);
            indexed = pipeline::typecheck(gs, move(indexed), opts, *workers);
        }

        if (opts.suggestTyped) {
            for (auto &tree : indexed) {
                auto file = tree.file;
                if (file.data(*gs).minErrorLevel() <= core::StrictLevel::Ignore) {
                    continue;
                }
                if (file.data(*gs).originalSigil > core::StrictLevel::Max) {
                    // don't change the sigil on "special" files
                    continue;
                }
                auto minErrorLevel = levelMinusOne(file.data(*gs).minErrorLevel());
                if (file.data(*gs).originalSigil == minErrorLevel) {
                    continue;
                }
                auto loc = findTyped(gs, file);
                if (auto e = gs->beginError(loc, core::errors::Infer::SuggestTyped)) {
                    auto sigil = levelToSigil(minErrorLevel);
                    e.setHeader("You could add `# typed: {}`", sigil);
                    e.addAutocorrect(core::AutocorrectSuggestion(loc, fmt::format("# typed: {}\n", sigil)));
                }
            }
        }

        gs->errorQueue->flushErrors(true);

        if (!opts.noErrorCount) {
            gs->errorQueue->flushErrorCount();
        }
        if (opts.autocorrect) {
            gs->errorQueue->flushAutocorrects(*gs, *opts.fs);
        }
        logger->trace("sorbet done");

        if (!opts.storeState.empty()) {
            gs->markAsPayload();
            FileOps::write(opts.storeState.c_str(), core::serialize::Serializer::store(*gs));
        }

        auto untypedSources = getAndClearHistogram("untyped.sources");
        if (opts.suggestSig) {
            ENFORCE(sorbet::debug_mode);
            vector<pair<string, int>> withNames;
            long sum = 0;
            for (auto e : untypedSources) {
                withNames.emplace_back(core::SymbolRef(*gs, e.first).dataAllowingNone(*gs)->showFullName(*gs),
                                       e.second);
                sum += e.second;
            }
            fast_sort(withNames, [](const auto &lhs, const auto &rhs) -> bool { return lhs.second > rhs.second; });
            for (auto &p : withNames) {
                logger->error("Typing `{}` would impact {}% callsites({} out of {}).", p.first, p.second * 100.0 / sum,
                              p.second, sum);
            }
        }
    }

    addStandardMetrics();

    if (!opts.someCounters.empty()) {
        if (opts.enableCounters) {
            logger->error("Don't pass both --counters and --counter");
            return 1;
        }
        logger->warn("" + getCounterStatistics(opts.someCounters));
    }

    if (opts.enableCounters) {
        logger->warn("" + getCounterStatistics(Counters::ALL_COUNTERS));
    } else {
        logger->debug("" + getCounterStatistics(Counters::ALL_COUNTERS));
    }

    auto counters = getAndClearThreadCounters();

    if (!opts.statsdHost.empty()) {
        auto prefix = opts.statsdPrefix;
        if (opts.runLSP) {
            prefix += ".lsp";
        }
        StatsD::submitCounters(counters, opts.statsdHost, opts.statsdPort, prefix + ".counters");
    }
    if (!opts.webTraceFile.empty()) {
        web_tracer_framework::Tracing::storeTraces(counters, opts.webTraceFile);
    }

    if (!opts.metricsFile.empty()) {
        auto metrics = core::Proto::toProto(counters, opts.metricsPrefix);
        string status;
        if (gs->hadCriticalError()) {
            status = "Error";
        } else if (returnCode != 0) {
            status = "Failure";
        } else {
            status = "Success";
        }

        metrics.set_repo(opts.metricsRepo);
        metrics.set_branch(opts.metricsBranch);
        metrics.set_sha(opts.metricsSha);
        metrics.set_status(status);

        auto json = core::Proto::toJSON(metrics);

        // Create output directory if it doesn't exist
        try {
            opts.fs->writeFile(opts.metricsFile, json);
        } catch (FileNotFoundException e) {
            logger->error("Cannot write metrics file at `{}`", opts.metricsFile);
        }
    }
    if (gs->hadCriticalError()) {
        returnCode = 10;
    } else if (returnCode == 0 && gs->totalErrors() > 0 && !opts.supressNonCriticalErrors) {
        returnCode = 1;
    }

    opts.flushPrinters();

    if (!sorbet::emscripten_build) {
        // Let it go: leak memory so that we don't need to call destructors
        for (auto &e : indexed) {
            intentionallyLeakMemory(e.tree.release());
        }
        intentionallyLeakMemory(gs.release());
    }

    // je_malloc_stats_print(nullptr, nullptr, nullptr); // uncomment this to print jemalloc statistics

    return returnCode;
}

} // namespace sorbet::realmain
