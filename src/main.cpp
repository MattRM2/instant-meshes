/*
    main.cpp -- Instant Meshes application entry point

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "batch.h"
#include "version.h"
#include "viewer.h"
#include "serializer.h"
#include <thread>
#include <cstdlib>

/* Force usage of discrete GPU on laptops */
NANOGUI_FORCE_DISCRETE_GPU();

int nprocs = -1;

int main(int argc, char **argv) {
    std::vector<std::string> args;
    bool extrinsic = true, dominant = false, align_to_boundaries = false;
    bool fullscreen = false, help = false, deterministic = false, compat = false;
    int rosy = 4, posy = 4, face_count = -1, vertex_count = -1;
    uint32_t knn_points = 10, smooth_iter = 2;
    Float crease_angle = -1, scale = -1, face_percent = -1;
    std::string batchOutput;
    std::vector<MeshRule> meshRules;
    FaceTarget others;
    bool listMeshes = false, dryRun = false, skipFailed = false, keepBorder = false, progress = false;
    int listSort = 0, listTop = 0;
    #if defined(__APPLE__)
        bool launched_from_finder = false;
    #endif

    try {
        for (int i=1; i<argc; ++i) {
            if (strcmp("--version", argv[i]) == 0 || strcmp("-V", argv[i]) == 0) {
                cout << INSTANT_MESHES_TITLE << " (MattRM2 fork, native Alembic)" << endl;
                return 0;
            } else if (strcmp("--fullscreen", argv[i]) == 0 || strcmp("-F", argv[i]) == 0) {
                fullscreen = true;
            } else if (strcmp("--help", argv[i]) == 0 || strcmp("-h", argv[i]) == 0) {
                help = true;
            } else if (strcmp("--deterministic", argv[i]) == 0 || strcmp("-d", argv[i]) == 0) {
                deterministic = true;
            } else if (strcmp("--intrinsic", argv[i]) == 0 || strcmp("-i", argv[i]) == 0) {
                extrinsic = false;
            } else if (strcmp("--boundaries", argv[i]) == 0 || strcmp("-b", argv[i]) == 0) {
                align_to_boundaries = true;
            } else if (strcmp("--threads", argv[i]) == 0 || strcmp("-t", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing thread count!" << endl;
                    return -1;
                }
                nprocs = str_to_uint32_t(argv[i]);
            } else if (strcmp("--smooth", argv[i]) == 0 || strcmp("-S", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing smoothing iteration count argument!" << endl;
                    return -1;
                }
                smooth_iter = str_to_uint32_t(argv[i]);
            } else if (strcmp("--knn", argv[i]) == 0 || strcmp("-k", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing knn point count argument!" << endl;
                    return -1;
                }
                knn_points = str_to_uint32_t(argv[i]);
            } else if (strcmp("--crease", argv[i]) == 0 || strcmp("-c", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing crease angle argument!" << endl;
                    return -1;
                }
                crease_angle = str_to_float(argv[i]);
            } else if (strcmp("--rosy", argv[i]) == 0 || strcmp("-r", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing rotation symmetry type!" << endl;
                    return -1;
                }
                rosy = str_to_int32_t(argv[i]);
            } else if (strcmp("--posy", argv[i]) == 0 || strcmp("-p", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing position symmetry type!" << endl;
                    return -1;
                }
                posy = str_to_int32_t(argv[i]);
                if (posy == 6)
                    posy = 3;
            } else if (strcmp("--scale", argv[i]) == 0 || strcmp("-s", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing scale argument!" << endl;
                    return -1;
                }
                scale = str_to_float(argv[i]);
            } else if (strcmp("--faces", argv[i]) == 0 || strcmp("-f", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing face count argument!" << endl;
                    return -1;
                }
                std::string value = argv[i];
                if (!value.empty() && value.back() == '%') {
                    face_percent = str_to_float(value.substr(0, value.size() - 1));
                    if (!std::isfinite(face_percent) || !(face_percent > 0))
                        throw std::runtime_error("Invalid face percentage \"" + value + "\"");
                } else {
                    face_count = str_to_int32_t(value);
                }
            } else if (strcmp("--vertices", argv[i]) == 0 || strcmp("-v", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing vertex count argument!" << endl;
                    return -1;
                }
                vertex_count = str_to_int32_t(argv[i]);
            } else if (strcmp("--output", argv[i]) == 0 || strcmp("-o", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing batch mode output file argument!" << endl;
                    return -1;
                }
                batchOutput = argv[i];
            } else if (strcmp("--dominant", argv[i]) == 0 || strcmp("-D", argv[i]) == 0) {
                dominant = true;
            } else if (strcmp("--mesh", argv[i]) == 0 || strcmp("-m", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing mesh rule argument (e.g. -m \"Mesh*=75%\")!" << endl;
                    return -1;
                }
                meshRules.push_back(parse_mesh_rule(argv[i]));
            } else if (strcmp("--others", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing --others target argument!" << endl;
                    return -1;
                }
                others = parse_face_target(argv[i]);
            } else if (strcmp("--list", argv[i]) == 0) {
                listMeshes = true;
            } else if (strcmp("--sort", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing --sort order (asc or desc)!" << endl;
                    return -1;
                }
                const std::string order = str_tolower(argv[i]);
                if (order == "asc")
                    listSort = 1;
                else if (order == "desc")
                    listSort = -1;
                else
                    throw std::runtime_error("Invalid --sort order \"" + std::string(argv[i]) + "\" (asc or desc)");
            } else if (strcmp("--top", argv[i]) == 0) {
                if (++i >= argc) {
                    cerr << "Missing --top count!" << endl;
                    return -1;
                }
                listTop = str_to_int32_t(argv[i]);
                if (listTop <= 0)
                    throw std::runtime_error("Invalid --top count \"" + std::string(argv[i]) + "\"");
            } else if (strcmp("--dry-run", argv[i]) == 0) {
                dryRun = true;
            } else if (strcmp("--skip-failed", argv[i]) == 0) {
                skipFailed = true;
            } else if (strcmp("--progress", argv[i]) == 0) {
                progress = true;
            } else if (strcmp("--keep-border", argv[i]) == 0) {
                keepBorder = true;
            } else if (strcmp("--compat", argv[i]) == 0 || strcmp("-C", argv[i]) == 0) {
                compat = true;
#if defined(__APPLE__)
            } else if (strncmp("-psn", argv[i], 4) == 0) {
                launched_from_finder = true;
#endif
            } else {
                if (strncmp(argv[i], "-", 1) == 0) {
                    cerr << "Invalid argument: \"" << argv[i] << "\"!" << endl;
                    help = true;
                }
                args.push_back(argv[i]);
            }
        }
    } catch (const std::exception &e) {
        cout << "Error: " << e.what() << endl;
        help = true;
    }

    if ((posy != 3 && posy != 4) || (rosy != 2 && rosy != 4 && rosy != 6)) {
        cerr << "Error: Invalid symmetry type!" << endl;
        help  = true;
    }

    int nConstraints = 0;
    nConstraints += scale > 0 ? 1 : 0;
    nConstraints += (face_count > 0 || face_percent > 0) ? 1 : 0;
    nConstraints += vertex_count > 0 ? 1 : 0;

    if (nConstraints > 1) {
        cerr << "Error: Only one of the --scale, --face and --vertices parameters can be used at once!" << endl;
        help = true;
    }


    /* Check the output format before spending time on the computation */
    if (!batchOutput.empty()) {
        std::string extension = batchOutput.size() > 4 ? str_tolower(batchOutput.substr(batchOutput.size() - 4)) : "";
        if (extension != ".obj" && extension != ".ply" && extension != ".abc") {
            cerr << "Error: unsupported output format \"" << batchOutput << "\" (.obj/.ply/.abc are supported)!" << endl;
            help = true;
        }
    }

    /* Alembic per-mesh modes */
    const bool objectMode = !meshRules.empty() || others.valid();
    auto extension_of = [](const std::string &f) {
        return f.size() > 4 ? str_tolower(f.substr(f.size() - 4)) : std::string();
    };
    const std::string sceneExt = args.size() == 1 ? extension_of(args[0]) : std::string();
    if ((listMeshes || objectMode || dryRun) && (sceneExt != ".abc" && sceneExt != ".obj")) {
        cerr << "Error: --list, -m, --others and --dry-run need one Alembic (.abc) or OBJ (.obj) input file!" << endl;
        help = true;
    }
    if ((listSort != 0 || listTop > 0) && !listMeshes) {
        cerr << "Error: --sort and --top apply to --list!" << endl;
        help = true;
    }
    if (dryRun && !objectMode) {
        cerr << "Error: --dry-run shows the plan of -m / --others rules!" << endl;
        help = true;
    }
    if (keepBorder && batchOutput.empty() && !dryRun) {
        cerr << "Error: --keep-border applies to the batch mode (-o <output>)!" << endl;
        help = true;
    }
    if (keepBorder && extension_of(batchOutput) == ".ply") {
        cerr << "Error: --keep-border needs an .obj or .abc output (its border faces are polygons)!" << endl;
        help = true;
    }
    if (progress && !objectMode) {
        cerr << "Error: --progress applies to the -m / --others per-mesh mode!" << endl;
        help = true;
    }
    if (skipFailed && !objectMode) {
        cerr << "Error: --skip-failed applies to the -m / --others per-mesh mode!" << endl;
        help = true;
    }
    if (objectMode && nConstraints > 0) {
        cerr << "Error: with -m / --others, give the face targets there (-f, -s and -v remesh the whole file)!" << endl;
        help = true;
    }
    if (objectMode && !dryRun && (sceneExt == ".abc" || sceneExt == ".obj") &&
        extension_of(batchOutput) != sceneExt) {
        cerr << "Error: -m / --others need an output file (-o) of the same format as the input ("
             << sceneExt << ")!" << endl;
        help = true;
    }

    if (args.size() > 1 || help || (!batchOutput.empty() && args.size() == 0)) {
        cout << INSTANT_MESHES_TITLE << " (MattRM2 fork)" << endl;
        cout << "Syntax: " << argv[0] << " [options] <input mesh / point cloud / application state snapshot>" << endl;
        cout << "Options:" << endl;
        cout << "   -o, --output <output>     Writes to the specified PLY/OBJ/ABC output file in batch mode" << endl;
        cout << "   -t, --threads <count>     Number of threads used for parallel computations" << endl;
        cout << "   -d, --deterministic       Prefer (slower) deterministic algorithms" << endl;
        cout << "   -c, --crease <degrees>    Dihedral angle threshold for creases" << endl;
        cout << "   -S, --smooth <iter>       Number of smoothing & ray tracing reprojection steps (default: 2)" << endl;
        cout << "   -D, --dominant            Generate a tri/quad dominant mesh instead of a pure tri/quad mesh" << endl;
        cout << "   -i, --intrinsic           Intrinsic mode (extrinsic is the default)" << endl;
        cout << "   -b, --boundaries          Align to boundaries (only applies when the mesh is not closed)" << endl;
        cout << "       --keep-border         Snap the open border back onto the input border (implies -b):" << endl;
        cout << "                             objects touching along their borders stay closed (.obj/.abc)" << endl;
        cout << "   -r, --rosy <number>     Specifies the orientation symmetry type (2, 4, or 6)" << endl;
        cout << "   -p, --posy <number>       Specifies the position symmetry type (4 or 6)" << endl;
        cout << "   -s, --scale <scale>       Desired world space length of edges in the output" << endl;
        cout << "   -f, --faces <count>       Desired face count of the output mesh (approximate)," << endl;
        cout << "       --faces <percent>%    or a percentage of the input polygon count, e.g. 75%" << endl;
        cout << "                             (mesh inputs; 100% = polygons of the file;" << endl;
        cout << "                             about +/-3%, less accurate below a few hundred polygons)" << endl;
        cout << "   -v, --vertices <count>    Desired vertex count of the output mesh" << endl;
        cout << "Per-mesh mode for Alembic (.abc) and OBJ (.obj) scenes (output in the same format):" << endl;
        cout << "   -m, --mesh <name>=<target>  Remesh the polygon meshes matching <name> on their own:" << endl;
        cout << "                             <target> = percentage (75%) or face count (5000);" << endl;
        cout << "                             <name> = object name or path (Props/MeshA), wildcards" << endl;
        cout << "                             * and ? (quote them: \"Mesh*=75%\"); repeatable, the last" << endl;
        cout << "                             matching -m wins" << endl;
        cout << "       --others <target>     Remesh every other polygon mesh with <target>" << endl;
        cout << "                             (without it, the other objects are copied unchanged)" << endl;
        cout << "       --dry-run             Print the plan of -m / --others and stop" << endl;
        cout << "       --skip-failed         A mesh that cannot be remeshed (e.g. no faces for its" << endl;
        cout << "                             target) is copied unchanged instead of stopping" << endl;
        cout << "       --progress            Print the progress after each remeshed mesh (in %" << endl;
        cout << "                             of the input faces, elapsed and remaining time)" << endl;
        cout << "       --list                List the polygon meshes of an .abc file / objects of an .obj" << endl;
        cout << "       --sort asc|desc       With --list: by ascending / descending face count" << endl;
        cout << "       --top <n>             With --list: only the first <n> (e.g. --sort desc --top 10)" << endl;
        cout << "   -C, --compat              Compatibility mode to load snapshots from old software versions" << endl;
        cout << "   -k, --knn <count>         Point cloud mode: number of adjacent points to consider" << endl;
        cout << "   -F, --fullscreen          Open a full-screen window" << endl;
        cout << "   -V, --version             Print the version" << endl;
        cout << "   -h, --help                Display this message" << endl;
        return -1;
    }

    if (args.size() == 0)
        cout << "Running in GUI mode, start with -h for instructions on batch mode." << endl;

    tbb::task_scheduler_init init(nprocs == -1 ? tbb::task_scheduler_init::automatic : nprocs);

    RemeshParams params;
    params.rosy = rosy;
    params.posy = posy;
    params.scale = scale;
    params.face_count = face_count;
    params.face_percent = face_percent;
    params.vertex_count = vertex_count;
    params.crease_angle = crease_angle;
    params.extrinsic = extrinsic;
    params.align_to_boundaries = align_to_boundaries;
    params.smooth_iter = smooth_iter;
    params.knn_points = knn_points;
    params.pure_quad = !dominant;
    params.deterministic = deterministic;
    params.keep_border = keepBorder;

    if (listMeshes || objectMode || (!batchOutput.empty() && args.size() == 1)) {
        try {
            if (listMeshes)
                batch_list(args[0], listSort, listTop);
            else if (objectMode)
                batch_process_objects(args[0], batchOutput, params, meshRules, others, dryRun, skipFailed, progress);
            else
                batch_process(args[0], batchOutput, params);
            return 0;
        } catch (const std::exception &e) {
            cerr << "Caught runtime error : " << e.what() << endl;
            return -1;
        }
    }

    try {
        nanogui::init();

        #if defined(__APPLE__)
            if (launched_from_finder)
                nanogui::chdir_to_bundle_parent();
        #endif

        {
            nanogui::ref<Viewer> viewer = new Viewer(fullscreen, deterministic);
            viewer->setVisible(true);

            if (args.size() == 1) {
                if (Serializer::isSerializedFile(args[0])) {
                    viewer->loadState(args[0], compat);
                } else {
                    viewer->loadInput(args[0], crease_angle,
                            scale, face_count, vertex_count,
                            rosy, posy, knn_points);
                    viewer->setExtrinsic(extrinsic);
                    if (face_percent > 0)
                        viewer->setTargetPercent(face_percent);
                }
            }

            nanogui::mainloop();
        }

        nanogui::shutdown();
    } catch (const std::runtime_error &e) {
        std::string error_msg = std::string("Caught a fatal error: ") + std::string(e.what());
        #if defined(_WIN32)
            MessageBoxA(nullptr, error_msg.c_str(), NULL, MB_ICONERROR | MB_OK);
        #else
            std::cerr << error_msg << endl;
        #endif
        return -1;
    }

    return EXIT_SUCCESS;
}
