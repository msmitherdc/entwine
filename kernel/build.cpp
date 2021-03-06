/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include "entwine.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include <entwine/formats/cesium/settings.hpp>
#include <entwine/third/arbiter/arbiter.hpp>
#include <entwine/tree/builder.hpp>
#include <entwine/tree/chunk.hpp>
#include <entwine/tree/config-parser.hpp>
#include <entwine/tree/thread-pools.hpp>
#include <entwine/types/bounds.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/reprojection.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/storage.hpp>
#include <entwine/types/structure.hpp>
#include <entwine/types/subset.hpp>
#include <entwine/util/json.hpp>
#include <entwine/util/matrix.hpp>

using namespace entwine;

namespace
{
    std::string yesNo(const bool val)
    {
        return (val ? "yes" : "no");
    }

    std::string getUsageString()
    {
        return
            "\nUsage: entwine build <config file> <options>\n"

            "\nConfig file:\n"
            "\tOptional parameter, recommended only if the options below are\n"
            "\tinsufficient.  See template at https://git.io/v2jPQ\n"

            "\nOptions (overrides config values):\n"

            "\t-i <input path>\n"
            "\t\tSpecify the input location.  May end in '/*' for a\n"
            "\t\tnon-recursive directory or '/**' for a recursive search.\n"
            "\t\tMay be type-prefixed, e.g. s3://bucket/data/*.\n\n"

            "\t-o <output path>\n"
            "\t\tOutput directory.\n\n"

            "\t-a <tmp path>\n"
            "\t\tDirectory for entwine-generated temporary files.\n\n"

            "\t-b [xmin, ymin, zmin, xmax, ymax, zmax]\n"
            "\t\tSet the boundings for the index.  Points outside of the\n"
            "\t\tgiven coordinates will be discarded.\n\n"

            "\t-r (<input reprojection>) <output reprojection>\n"
            "\t\tSet the spatial reference system reprojection.  The input\n"
            "\t\tvalue may be omitted to infer the input SRS from the file\n"
            "\t\theader.  In this case the build will fail if no input SRS\n"
            "\t\tmay be inferred.  Reprojection strings may be any of the\n"
            "\t\tformats supported by GDAL.\n\n"
            "\t\tIf an input reprojection is supplied, by default it will\n"
            "\t\tonly be used when no SRS can be inferred from the file.  To\n"
            "\t\toverride this behavior and use the specified input SRS even\n"
            "\t\twhen one can be found from the file header, set the '-h'\n"
            "\t\tflag.\n\n"

            "\t-h\n"
            "\t\tIf set, the user-supplied input SRS will always override\n"
            "\t\tany SRS inferred from file headers.\n\n"

            "\t-t <threads>\n"
            "\t\tSet the number of worker threads.  Recommended to be no\n"
            "\t\tmore than the physical number of cores.\n\n"

            "\t-f\n"
            "\t\tForce build overwrite - do not continue a previous build\n"
            "\t\tthat may exist at this output location.\n\n"

            "\t-u <aws user>\n"
            "\t\tSpecify AWS credential user, if not default\n\n"

            "\t-e\n"
            "\t\tEnable AWS server-side-encryption.\n\n"

            "\t-x\n"
            "\t\tDo not trust file headers when determining bounds.  By\n"
            "\t\tdefault, the headers are considered to be good.\n\n"

            "\t-c <compression-type>\n"
            "\t\tSet data storage type.  Valid value: 'none', 'laszip',\n"
            "\t\tor 'lazperf'.\n\n"

            "\t-n\n"
            "\t\tIf set, absolute positioning will be used, even if values\n"
            "\t\tfor scale/offset can be inferred.\n\n"

            "\t-s <scale>\n"
            "\t\tSet a scale factor for indexed output.\n\n"

            "\t-s <subset-number> <subset-total>\n"
            "\t\tBuild only a portion of the index.  If output paths are\n"
            "\t\tall the same, 'merge' should be run after all subsets are\n"
            "\t\tbuilt.  If output paths are different, then 'link' should\n"
            "\t\tbe run after all subsets are built.\n\n"
            "\t\tsubset-number - One-based subset ID in range\n"
            "\t\t[1, subset-total].\n\n"
            "\t\tsubset-total - Total number of subsets that will be built.\n"
            "\t\tMust be a binary power.\n\n"

            "\t-m <JSON-array>\n"
            "\t\tTransformation matrix.\n\n"

            "\t-d <density>\n"
            "\t\tDensity estimate, in points per square unit\n\n";
    }

    std::string getDimensionString(const Schema& schema)
    {
        const DimList dims(schema.dims());
        std::string results("[\n\t\t");

        for (std::size_t i(0); i < dims.size(); ++i)
        {
            if (i)
            {
                if (i % 5 == 0) results += "\n\t\t";
                else results += ", ";
            }
            results += dims[i].name();
        }

        results += "\n\t]";

        return results;
    }

    std::string getReprojString(const Reprojection* reprojection)
    {
        if (reprojection)
        {
            std::string s;

            if (reprojection->hammer())
            {
                s += reprojection->in() + " (OVERRIDING file headers)";
            }
            else
            {
                if (reprojection->in().size())
                {
                    s += "(from file headers, or a default of '";
                    s += reprojection->in();
                    s += "')";
                }
                else
                {
                    s += "(from file headers)";
                }
            }

            s += " -> ";
            s += reprojection->out();

            return s;
        }
        else
        {
            return "(none)";
        }
    }
}

void Kernel::build(std::vector<std::string> args)
{
    if (args.empty())
    {
        std::cout << getUsageString() << std::flush;
        return;
    }

    if (args.size() == 1)
    {
        if (args[0] == "help" || args[0] == "-h" || args[0] == "--help")
        {
            std::cout << getUsageString() << std::flush;
            return;
        }
    }

    Json::Value json(ConfigParser::defaults());
    entwine::arbiter::Arbiter localArbiter;

    std::size_t a(0);

    if (args[0].front() != '-')
    {
        // First argument is a config path.
        const std::string configPath(args[0]);
        const Json::Value config(parse(localArbiter.get(configPath)));

        recMerge(json, config);

        ++a;
    }

    auto error([](const std::string& message)
    {
        throw std::runtime_error(message);
    });

    while (a < args.size())
    {
        const std::string arg(args[a]);

        if (arg == "-i")
        {
            ++a;
            auto i(a);

            while (i < args.size() && args[i].front() != '-')
            {
                ++i;
            }

            if (i == a || i > args.size())
            {
                error("Invalid input path specification");
            }
            else if (i == a + 1)
            {
                json["input"] = args[a];
            }
            else
            {
                while (a < i)
                {
                    json["input"].append(args[a]);
                    ++a;
                }

                --a;
            }
        }
        else if (arg == "-o")
        {
            if (++a < args.size()) json["output"] = args[a];
            else error("Invalid output path specification");
        }
        else if (arg == "-c")
        {
            if (++a < args.size()) json["compression"] = args[a];
            else error("Invalid compression specification");
        }
        else if (arg == "-a")
        {
            if (++a < args.size()) json["tmp"] = args[a];
            else error("Invalid tmp specification");
        }
        else if (arg == "-b")
        {
            std::string str;
            bool done(false);

            while (!done && ++a < args.size())
            {
                str += args[a];
                if (args[a].find(']') != std::string::npos) done = true;
            }

            if (done) json["bounds"] = parse(str);
            else error("Invalid bounds: " + str);
        }
        else if (arg == "-f") { json["force"] = true; }
        else if (arg == "-x") { json["trustHeaders"] = false; }
        else if (arg == "-n") { json["absolute"] = true; }
        else if (arg == "-e") { json["arbiter"]["s3"]["sse"] = true; }
        else if (arg == "-h")
        {
            json["reprojection"]["hammer"] = true;
        }
        else if (arg == "-s")
        {
            if (++a < args.size())
            {
                // If there's only one following argument, then this is a
                // scale specification.  Otherwise, it's a subset specification.
                if (a + 1 >= args.size() || args[a + 1].front() == '-')
                {
                    if (args[a].front() == '[')
                    {
                        json["scale"] = parse(args[a]);
                    }
                    else
                    {
                        const double d(std::stod(args[a]));
                        for (int i(0); i < 3; ++i) json["scale"].append(d);
                    }
                }
                else
                {
                    const Json::UInt64 id(std::stoul(args[a]));
                    const Json::UInt64 of(std::stoul(args[++a]));

                    json["subset"]["id"] = id;
                    json["subset"]["of"] = of;
                }
            }
            else
            {
                error("Invalid -s specification");
            }
        }
        else if (arg == "-u")
        {
            if (++a < args.size())
            {
                json["arbiter"]["s3"]["profile"] = args[a];
            }
            else
            {
                error("Invalid AWS user argument");
            }
        }
        else if (arg == "-g")
        {
            if (++a < args.size())
            {
                json["run"] = Json::UInt64(std::stoul(args[a]));
            }
            else error("Invalid run-count argument");
        }
        else if (arg == "-r")
        {
            if (++a < args.size())
            {
                const bool onlyOutput(
                        a + 1 >= args.size() ||
                        args[a + 1].front() == '-');

                if (onlyOutput)
                {
                    json["reprojection"]["out"] = args[a];
                }
                else
                {
                    json["reprojection"]["in"] = args[a];
                    json["reprojection"]["out"] = args[++a];
                }
            }
            else
            {
                error("Invalid reprojection argument");
            }
        }
        else if (arg == "-h")
        {
            json["reprojection"]["hammer"] = true;
        }
        else if (arg == "-t")
        {
            if (++a < args.size())
            {
                json["threads"] = parse(args[a]);
            }
            else
            {
                error("Invalid thread count specification");
            }
        }
        else if (arg == "-m")
        {
            if (++a < args.size())
            {
                json["transformation"] = parse(args[a]);
                if (json["transformation"].size() != 16)
                {
                    throw std::runtime_error("Invalid transformation matrix");
                }
            }
            else error("Invalid transformation matrix");
        }
        else if (arg == "-d")
        {
            if (++a < args.size())
            {
                json["density"] = parse(args[a]);
            }
            else error("Invalid density specification");
        }
        else if (arg == "-p")
        {
            if (++a < args.size())
            {
                json["preserveSpatial"] = parse(args[a]);
            }
            else error("Invalid preserveSpatial specification");
        }
        else
        {
            error("Invalid argument: " + args[a]);
        }

        ++a;
    }

    auto arbiter(std::make_shared<entwine::arbiter::Arbiter>(json["arbiter"]));

    json["verbose"] = true;
    std::unique_ptr<Builder> builder(ConfigParser::getBuilder(json, arbiter));

    if (builder->isContinuation())
    {
        std::cout << "\nContinuing previous index..." << std::endl;
    }

    const auto& outEndpoint(builder->outEndpoint());
    const auto& tmpEndpoint(builder->tmpEndpoint());

    std::string outPath(
            (outEndpoint.type() != "file" ? outEndpoint.type() + "://" : "") +
            outEndpoint.root());
    std::string tmpPath(tmpEndpoint.root());

    const Metadata& metadata(builder->metadata());
    const Structure& structure(metadata.structure());
    const Manifest& manifest(metadata.manifest());

    const Reprojection* reprojection(metadata.reprojection());
    const Schema& schema(metadata.schema());
    const std::size_t runCount(json["run"].asUInt64());

    std::cout << std::endl;
    std::cout <<
        "Version: " << currentVersion().toString() << "\n" <<
        "Input:\n\t";

    if (manifest.size() == 1)
    {
        std::cout << "File: " << manifest.get(0).path() << std::endl;
    }
    else
    {
        std::cout << "Files: " << manifest.size() << std::endl;
    }

    if (const Subset* subset = metadata.subset())
    {
        std::cout <<
            "\tSubset: " <<
                subset->id() + 1 << " of " <<
                subset->of() << "\n" <<
            "\tSubset bounds: " << subset->bounds() <<
            std::endl;
    }

    if (runCount)
    {
        std::cout <<
            "\tInserting up to " << runCount << " file" <<
            (runCount > 1 ? "s" : "") << "\n";
    }

    const Storage& storage(metadata.storage());

    const std::string coldDepthString(
            structure.lossless() ?
                "lossless" :
                std::to_string(structure.coldDepthEnd()));

    const auto& threadPools(builder->threadPools());

    std::cout <<
        "\tPoint count hint: " << commify(structure.numPointsHint()) <<
            " points\n";

    std::cout << "\tDensity estimate (per square unit): ";
    if (metadata.density()) std::cout << metadata.density() << std::endl;
    else std::cout << densityLowerBound(metadata.manifest().fileInfo()) <<
        std::endl;

    if (!metadata.trustHeaders())
    {
        std::cout << "\tTrust file headers? " << yesNo(false) << "\n";
    }

    std::cout <<
        "\tThreads: " << threadPools.size() <<
        std::endl;

    std::cout <<
        "Output:\n" <<
        "\tOutput path: " << outPath << "\n" <<
        "\tData storage: " << toString(storage.chunkStorageType()) <<
        std::endl;

    if (const auto* delta = metadata.delta())
    {
        std::cout << "\tScale: ";
        const auto& scale(delta->scale());
        if (scale.x == scale.y && scale.x == scale.z)
        {
            std::cout << scale.x << std::endl;
        }
        else
        {
            std::cout << scale << std::endl;
        }
        std::cout << "\tOffset: " << delta->offset() << std::endl;
        std::cout << "\tXYZ width: " << schema.find("X").size() << std::endl;
    }

    std::cout <<
        "Metadata:\n" <<
        "\tNative bounds: " << metadata.boundsNativeConforming() << "\n" <<
        "\tCubic bounds: " << metadata.boundsNativeCubic() << "\n" <<
        "\tReprojection: " << getReprojString(reprojection) << "\n" <<
        "\tStoring dimensions: " << getDimensionString(schema) <<
        std::endl;

    if (metadata.transformation())
    {
        std::cout << "\tTransformation: ";
        matrix::print(*metadata.transformation(), 0, "\t");
    }

    if (const auto c = metadata.cesiumSettings())
    {
        std::cout <<
            "Cesium:\n" <<
            "\tTileset split depth: " << c->tilesetSplit() << "\n" <<
            "\tGeometric error divisor: " <<
                c->geometricErrorDivisor() << "\n" <<
            "\tTruncating colors: " << yesNo(c->truncate()) <<
            std::endl;

        if (c->coloring().size())
        {
            std::cout << "\tColoring: " << c->coloring() << std::endl;
        }
    }

    std::cout << std::endl;

    auto start = now();
    const std::size_t alreadyInserted(manifest.pointStats().inserts());

    builder->go(runCount);

    std::cout << "\nIndex completed in " <<
        commify(since<std::chrono::seconds>(start)) << " seconds." <<
        std::endl;

    std::cout << "Save complete.  Indexing stats:\n";

    const PointStats stats(manifest.pointStats());

    if (alreadyInserted)
    {
        std::cout <<
            "\tPoints inserted:\n" <<
            "\t\tPreviously: " << commify(alreadyInserted) << "\n" <<
            "\t\tCurrently:  " <<
                commify((stats.inserts() - alreadyInserted)) << "\n" <<
            "\t\tTotal:      " << commify(stats.inserts()) << std::endl;
    }
    else
    {
        std::cout << "\tPoints inserted: " << commify(stats.inserts()) << "\n";
    }

    std::cout <<
        "\tPoints discarded:\n" <<
        "\t\tOutside specified bounds: " <<
            commify(stats.outOfBounds()) << "\n" <<
        "\t\tOverflow past max depth: " <<
            commify(stats.overflows()) << "\n" <<
        std::endl;
}

