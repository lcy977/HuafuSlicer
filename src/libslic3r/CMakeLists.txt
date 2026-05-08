cmake_minimum_required(VERSION 3.13)
project(libslic3r)

include(PrecompiledHeader)

if(NOT DEFINED ORCA_CHECK_GCODE_PLACEHOLDERS)
    set(ORCA_CHECK_GCODE_PLACEHOLDERS "0")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)




if(NOT DEFINED ORCA_CHECK_GCODE_PLACEHOLDERS)
    set(ORCA_CHECK_GCODE_PLACEHOLDERS "0")
endif()

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/../third_party/local")
set(Boost_NO_SYSTEM_PATHS TRUE)
set(Boost_DEBUG ON)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem system nowide locale log iostreams)

find_package(TBB REQUIRED COMPONENTS tbb tbbmalloc)

set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON)
find_package(CGAL REQUIRED)

if(NOT TARGET mcut)
  set(MCUT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(MCUT_BUILD_TUTORIALS OFF CACHE BOOL "" FORCE)
  set(MCUT_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/mcut" "${CMAKE_BINARY_DIR}/mcut")
endif()

if(NOT TARGET qhullcpp)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/qhull" "${CMAKE_BINARY_DIR}/third_party/qhull")
endif()

if(NOT TARGET Clipper2)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/clipper2" "${CMAKE_BINARY_DIR}/third_party/clipper2")
endif()

# ClipperLib / ClipperLib_Z (clipper.cpp + clipper_z.cpp) — used by PerimeterGenerator, Support, etc.
if(NOT TARGET clipper)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/clipper" "${CMAKE_BINARY_DIR}/third_party/clipper")
endif()

if(NOT TARGET glu-libtess)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/glu-libtess" "${CMAKE_BINARY_DIR}/third_party/glu-libtess")
endif()

# libnoise (FuzzySkin: Perlin / Billow / RidgedMulti / Voronoi) — target libnoise_static, ALIAS noise::noise
if(NOT TARGET libnoise_static)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../third_party/libnoise" "${CMAKE_BINARY_DIR}/third_party/libnoise")
endif()

find_package(JPEG REQUIRED COMPONENTS JPEG)
# Draco: root CMake may build google/draco via FetchContent (draco::draco). Otherwise third_party/local config.
if(NOT TARGET draco::draco)
  find_package(draco CONFIG REQUIRED)
endif()

find_package(EXPAT REQUIRED CONFIG)

find_package(ZLIB REQUIRED)
# LibPNG built with PNG_EXPORT_SUFFIX / pnglibconf prefixes (prusaslicer_png_*); must match third_party/local/include/libpng16/pnglibconf.h
include("${CMAKE_CURRENT_LIST_DIR}/../third_party/local/lib/libpng/libpng16.cmake")

include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/eigen)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/semver)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/clipper)

include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/dep_PNG)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/TBB/include)
#include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/Cereal/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/libigl)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/expat)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/zlib)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/qhull/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/dep_PNG/libpng16)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party)
#include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/local/include/boost-1_84)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/libnest2d/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/clipper2/Clipper2Lib/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../libslic3r)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/local/include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/miniz)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/glu-libtess/include)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../third_party/miniz/miniz.c PROPERTIES LANGUAGE C)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../third_party/semver/semver.c PROPERTIES LANGUAGE C)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../third_party/nanosvg/nanosvg.c PROPERTIES LANGUAGE C)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../third_party/qoi/qoi.c PROPERTIES LANGUAGE C)
# Vendored libnoise: headers are under third_party/libnoise/src (noise.h, module/*.h), not libnoise/noise.h under local/include.
include_directories(${CMAKE_CURRENT_LIST_DIR}/../third_party/libnoise/src)

option(BUILD_SHARED_LIBS "Build shared libs" OFF)

# Prefer Boost bundled under src/third_party when available.
# Note: This repository vendors Boost sources, but the canonical headers/libs come from an install prefix.
# If Boost was built via src/third_party/Boost/build, it leaves an install manifest we can parse to infer the prefix.


set(lisbslic3r_sources
    AABBMesh.cpp
    AABBMesh.hpp
    AABBTreeIndirect.hpp
    AABBTreeLines.hpp
    Algorithm/LineSplit.cpp
    Algorithm/LineSplit.hpp
    Algorithm/RegionExpansion.cpp
    Algorithm/RegionExpansion.hpp
    AnyPtr.hpp
    AppConfig.cpp
    AppConfig.hpp
    Arachne/BeadingStrategy/BeadingStrategy.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.cpp
    Arachne/BeadingStrategy/BeadingStrategyFactory.hpp
    Arachne/BeadingStrategy/BeadingStrategy.hpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.cpp
    Arachne/BeadingStrategy/DistributedBeadingStrategy.hpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.cpp
    Arachne/BeadingStrategy/LimitedBeadingStrategy.hpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.cpp
    Arachne/BeadingStrategy/OuterWallInsetBeadingStrategy.hpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.cpp
    Arachne/BeadingStrategy/RedistributeBeadingStrategy.hpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.cpp
    Arachne/BeadingStrategy/WideningBeadingStrategy.hpp
    Arachne/SkeletalTrapezoidation.cpp
    Arachne/SkeletalTrapezoidationEdge.hpp
    Arachne/SkeletalTrapezoidationGraph.cpp
    Arachne/SkeletalTrapezoidationGraph.hpp
    Arachne/SkeletalTrapezoidation.hpp
    Arachne/SkeletalTrapezoidationJoint.hpp
    Arachne/utils/ExtrusionJunction.hpp
    Arachne/utils/ExtrusionLine.cpp
    Arachne/utils/ExtrusionLine.hpp
    Arachne/utils/HalfEdgeGraph.hpp
    Arachne/utils/HalfEdge.hpp
    Arachne/utils/HalfEdgeNode.hpp
    Arachne/utils/PolygonsPointIndex.hpp
    Arachne/utils/PolygonsSegmentIndex.hpp
    Arachne/utils/PolylineStitcher.cpp
    Arachne/utils/PolylineStitcher.hpp
    Arachne/utils/SparseGrid.hpp
    Arachne/utils/SparseLineGrid.hpp
    Arachne/utils/SparsePointGrid.hpp
    Arachne/utils/SquareGrid.cpp
    Arachne/utils/SquareGrid.hpp
    Arachne/WallToolPaths.cpp
    Arachne/WallToolPaths.hpp
    ArcFitter.cpp
    ArcFitter.hpp
    Arrange.cpp
    Arrange.hpp
    # admesh: Orca/Slic3r fork includes libslic3r headers — compile into libslic3r, not a separate lib (avoids inverted deps).
    admesh/connect.cpp
    admesh/normals.cpp
    admesh/shared.cpp
    admesh/stl_io.cpp
    admesh/stlinit.cpp
    admesh/util.cpp
    BlacklistedLibraryCheck.cpp
    BlacklistedLibraryCheck.hpp
    BoundingBox.cpp
    BoundingBox.hpp
    BridgeDetector.cpp
    BridgeDetector.hpp
    Brim.cpp
    BrimEarsPoint.hpp
    Brim.hpp
    BuildVolume.cpp
    BuildVolume.hpp
    calib.cpp
    calib.hpp
    Circle.cpp
    Circle.hpp
    clipper.cpp
    clipper.hpp
    ClipperUtils.cpp
    ClipperUtils.hpp
    Clipper2Utils.cpp
    Clipper2Utils.hpp
    ClipperZUtils.hpp
    Color.cpp
    Color.hpp
    CommonDefs.hpp
    Config.cpp
    Config.hpp
    CustomGCode.cpp
    CustomGCode.hpp
    CutUtils.cpp
    CutUtils.hpp
    EdgeGrid.cpp
    EdgeGrid.hpp
    ElephantFootCompensation.cpp
    ElephantFootCompensation.hpp
    Emboss.cpp
    Emboss.hpp
    EmbossShape.hpp
    enum_bitmask.hpp
    Execution/Execution.hpp
    Execution/ExecutionSeq.hpp
    Execution/ExecutionTBB.hpp
    ExPolygon.cpp
    ExPolygon.hpp
    ExPolygonSerialize.hpp
    ExPolygonsIndex.cpp
    ExPolygonsIndex.hpp
    Extruder.cpp
    Extruder.hpp
    ExtrusionEntityCollection.cpp
    ExtrusionEntityCollection.hpp
    ExtrusionEntity.cpp
    ExtrusionEntity.hpp
    ExtrusionSimulator.cpp
    ExtrusionSimulator.hpp
    FaceDetector.cpp
    FaceDetector.hpp
    Feature/FuzzySkin/FuzzySkin.cpp
    Feature/FuzzySkin/FuzzySkin.hpp
    Feature/Interlocking/InterlockingGenerator.cpp
    Feature/Interlocking/InterlockingGenerator.hpp
    Feature/Interlocking/VoxelUtils.cpp
    Feature/Interlocking/VoxelUtils.hpp
    FileParserError.hpp
    Fill/Fill3DHoneycomb.cpp
    Fill/Fill3DHoneycomb.hpp
    Fill/FillAdaptive.cpp
    Fill/FillAdaptive.hpp
    Fill/FillBase.cpp
    Fill/FillBase.hpp
    Fill/FillConcentric.cpp
    Fill/FillConcentric.hpp
    Fill/FillConcentricInternal.cpp
    Fill/FillConcentricInternal.hpp
    Fill/Fill.cpp
    Fill/FillCrossHatch.cpp
    Fill/FillCrossHatch.hpp
    Fill/FillGyroid.cpp
    Fill/FillGyroid.hpp
    Fill/FillHoneycomb.cpp
    Fill/FillHoneycomb.hpp
    Fill/Fill.hpp
    Fill/FillLightning.cpp
    Fill/FillLightning.hpp
    Fill/FillLine.cpp
    Fill/FillLine.hpp
    Fill/FillPlanePath.cpp
    Fill/FillPlanePath.hpp
    Fill/FillRectilinear.cpp
    Fill/FillRectilinear.hpp
    Fill/FillTpmsD.cpp
    Fill/FillTpmsD.hpp
    Fill/FillTpmsFK.cpp
    Fill/FillTpmsFK.hpp
    Fill/Lightning/DistanceField.cpp
    Fill/Lightning/DistanceField.hpp
    Fill/Lightning/Generator.cpp
    Fill/Lightning/Generator.hpp
    Fill/Lightning/Layer.cpp
    Fill/Lightning/Layer.hpp
    Fill/Lightning/TreeNode.cpp
    Fill/Lightning/TreeNode.hpp
    Flow.cpp
    Flow.hpp
    FlushVolCalc.cpp
    FlushVolCalc.hpp
    Format/3mf.cpp
    Format/3mf.hpp
    Format/AMF.cpp
    Format/AMF.hpp
    Format/DRC.cpp
    Format/DRC.hpp
    Format/bbs_3mf.cpp
    Format/bbs_3mf.hpp
    format.hpp
    Format/OBJ.cpp
    Format/OBJ.hpp
    Format/objparser.cpp
    Format/objparser.hpp
    Format/SL1.cpp
    Format/SL1.hpp
    Format/STL.cpp
    Format/STL.hpp
    Format/ZipperArchiveImport.cpp
    Format/ZipperArchiveImport.hpp
    GCode/AdaptivePAInterpolator.cpp
    GCode/AdaptivePAInterpolator.hpp
    GCode/AdaptivePAProcessor.cpp
    GCode/AdaptivePAProcessor.hpp
    GCode/AvoidCrossingPerimeters.cpp
    GCode/AvoidCrossingPerimeters.hpp
    GCode/ConflictChecker.cpp
    GCode/ConflictChecker.hpp
    GCode/CoolingBuffer.cpp
    GCode/CoolingBuffer.hpp
    GCode/TimelapsePosPicker.cpp
    GCode/TimelapsePosPicker.hpp
    GCode.cpp
    GCode/ExtrusionProcessor.hpp
    GCode/FanMover.cpp
    GCode/FanMover.hpp
    GCode/GCodeProcessor.cpp
    GCode/GCodeProcessor.hpp
    GCode.hpp
    GCode/PchipInterpolatorHelper.cpp
    GCode/PchipInterpolatorHelper.hpp
    GCode/PostProcessor.cpp
    GCode/PostProcessor.hpp
    GCode/PressureEqualizer.cpp
    GCode/PressureEqualizer.hpp
    GCode/PrintExtents.cpp
    GCode/PrintExtents.hpp
    GCodeReader.cpp
    GCodeReader.hpp
    GCode/RetractWhenCrossingPerimeters.cpp
    GCode/RetractWhenCrossingPerimeters.hpp
    GCode/SeamPlacer.cpp
    GCode/SeamPlacer.hpp
    #GCodeSender.cpp
    #GCodeSender.hpp
    GCode/SmallAreaInfillFlowCompensator.cpp
    GCode/SmallAreaInfillFlowCompensator.hpp
    GCode/SpiralVase.cpp
    GCode/SpiralVase.hpp
    GCode/ThumbnailData.cpp
    GCode/ThumbnailData.hpp
    GCode/Thumbnails.cpp
    GCode/Thumbnails.hpp
    GCode/ToolOrdering.cpp
    GCode/ToolOrdering.hpp
    GCode/WipeTower2.cpp
    GCode/WipeTower2.hpp
    GCode/WipeTower.cpp
    GCode/WipeTower.hpp
    GCodeWriter.cpp
    GCodeWriter.hpp
    Geometry/ArcWelder.hpp
    Geometry/ArcWelder.cpp
    Geometry/Bicubic.hpp
    Geometry/Circle.cpp
    Geometry/Circle.hpp
    Geometry/ConvexHull.cpp
    Geometry/ConvexHull.hpp
    Geometry.cpp
    Geometry/Curves.hpp
    Geometry.hpp
    Geometry/MedialAxis.cpp
    Geometry/MedialAxis.hpp
    Geometry/Voronoi.cpp
    Geometry/Voronoi.hpp
    Geometry/VoronoiOffset.cpp
    Geometry/VoronoiOffset.hpp
    Geometry/VoronoiUtilsCgal.cpp
    Geometry/VoronoiUtilsCgal.hpp
    Geometry/VoronoiUtils.cpp
    Geometry/VoronoiUtils.hpp
    Geometry/VoronoiVisualUtils.hpp
    Int128.hpp
    IntersectionPoints.cpp
    IntersectionPoints.hpp
    KDTreeIndirect.hpp
    Layer.cpp
    Layer.hpp
    LayerRegion.cpp
    libslic3r.cpp
    libslic3r.h
    Line.cpp
    Line.hpp
    LocalesUtils.cpp
    LocalesUtils.hpp
    MarchingSquares.hpp
    Measure.cpp
    Measure.hpp
    MeasureUtils.hpp
    MeshBoolean.cpp
    MeshBoolean.hpp
    MeshSplitImpl.hpp
    MinAreaBoundingBox.cpp
    MinAreaBoundingBox.hpp
    MinimumSpanningTree.cpp
    MinimumSpanningTree.hpp
    miniz_extension.cpp
    miniz_extension.hpp
    ../third_party/miniz/miniz.c
    ModelArrange.cpp
    ModelArrange.hpp
    Model.cpp
    Model.hpp
    MTUtils.hpp
    MultiMaterialSegmentation.cpp
    MultiMaterialSegmentation.hpp
    MultiPoint.cpp
    MultiPoint.hpp
    MutablePolygon.cpp
    MutablePolygon.hpp
    MutablePriorityQueue.hpp
    NormalUtils.cpp
    NormalUtils.hpp
    NSVGUtils.cpp
    NSVGUtils.hpp
    ../third_party/nanosvg/nanosvg.c
    ../third_party/qoi/qoi.c
    ObjectID.cpp
    ObjectID.hpp
    Optimize/BruteforceOptimizer.hpp
    Optimize/NLoptOptimizer.hpp
    Optimize/Optimizer.hpp
    Orient.cpp
    Orient.hpp
    ParameterUtils.cpp
    ParameterUtils.hpp
    pchheader.cpp
    pchheader.hpp
    PerimeterGenerator.cpp
    PerimeterGenerator.hpp
    PlaceholderParser.cpp
    PlaceholderParser.hpp
    Platform.cpp
    Platform.hpp
    PNGReadWrite.cpp
    PNGReadWrite.hpp
    Point.cpp
    Point.hpp
    Polygon.cpp
    Polygon.hpp
    PolygonTrimmer.cpp
    PolygonTrimmer.hpp
    Polyline.cpp
    Polyline.hpp
    PresetBundle.cpp
    PresetBundle.hpp
    Preset.cpp
    Preset.hpp
    PrincipalComponents2D.cpp
    PrincipalComponents2D.hpp
    PrintApply.cpp
    PrintBase.cpp
    PrintBase.hpp
    MaterialType.cpp
    MaterialType.hpp
    PrintConfig.cpp
    PrintConfig.hpp
    Print.cpp
    Print.hpp
    PrintObject.cpp
    PrintObjectSlice.cpp
    PrintRegion.cpp
    ProjectTask.cpp
    ProjectTask.hpp
    QuadricEdgeCollapse.cpp
    QuadricEdgeCollapse.hpp
    Semver.cpp
    ../third_party/semver/semver.c
    ShortEdgeCollapse.cpp
    ShortEdgeCollapse.hpp
    ShortestPath.cpp
    ShortestPath.hpp
    SLA/AGGRaster.hpp
    SLA/BoostAdapter.hpp
    SLA/Clustering.cpp
    SLA/Clustering.hpp
    SLA/ConcaveHull.cpp
    SLA/ConcaveHull.hpp
    SLA/Concurrency.hpp
    SLA/Hollowing.cpp
    SLA/Hollowing.hpp
    SLA/IndexedMesh.cpp
    SLA/IndexedMesh.hpp
    SLA/JobController.hpp
    SLA/Pad.cpp
    SLA/Pad.hpp
    SLAPrint.cpp
    SLAPrint.hpp
    SLAPrintSteps.cpp
    SLAPrintSteps.hpp
    SLA/RasterBase.cpp
    SLA/RasterBase.hpp
    SLA/RasterToPolygons.cpp
    SLA/RasterToPolygons.hpp
    SLA/ReprojectPointsOnMesh.hpp
    SLA/Rotfinder.cpp
    SLA/Rotfinder.hpp
    SLA/SpatIndex.cpp
    SLA/SpatIndex.hpp
    SLA/SupportPointGenerator.cpp
    SLA/SupportPointGenerator.hpp
    SLA/SupportPoint.hpp
    SLA/SupportTreeBuilder.cpp
    SLA/SupportTreeBuilder.hpp
    SLA/SupportTreeBuildsteps.cpp
    SLA/SupportTreeBuildsteps.hpp
    SLA/SupportTree.cpp
    SLA/SupportTree.hpp
    #SLA/SupportTreeIGL.cpp
    SLA/SupportTreeMesher.cpp
    SLA/SupportTreeMesher.hpp
    SlicesToTriangleMesh.cpp
    SlicesToTriangleMesh.hpp
    SlicingAdaptive.cpp
    SlicingAdaptive.hpp
    Slicing.cpp
    Slicing.hpp
    Support/SupportCommon.cpp
    Support/SupportCommon.hpp
    Support/SupportLayer.hpp
    Support/SupportMaterial.cpp
    Support/SupportMaterial.hpp
    Support/SupportParameters.hpp
    Support/SupportSpotsGenerator.cpp
    Support/SupportSpotsGenerator.hpp
    Support/TreeModelVolumes.cpp
    Support/TreeModelVolumes.hpp
    Support/TreeSupport3D.cpp
    Support/TreeSupport3D.hpp
    Support/TreeSupportCommon.hpp
    Support/TreeSupport.cpp
    Support/TreeSupport.hpp
    SurfaceCollection.cpp
    SurfaceCollection.hpp
    Surface.cpp
    Surface.hpp
    SurfaceMesh.hpp
    SVG.cpp
    SVG.hpp
    Technologies.hpp
    Tesselate.cpp
    Tesselate.hpp
    TextConfiguration.hpp
    Thread.cpp
    Thread.hpp
    Time.cpp
    Time.hpp
    Timer.cpp
    Timer.hpp
    Triangulation.cpp
    Triangulation.hpp
    TriangleMesh.cpp
    TriangleMesh.hpp
    TriangleMeshDeal.cpp
    TriangleMeshDeal.hpp
    TriangleMeshSlicer.cpp
    TriangleMeshSlicer.hpp
    TriangleSelector.cpp
    TriangleSelector.hpp
    TriangleSetSampling.cpp
    TriangleSetSampling.hpp
    TriangulateWall.cpp
    TriangulateWall.hpp
    TryCatchSignal.cpp
    TryCatchSignal.hpp
    utils.cpp
    Utils.hpp
    VariableWidth.cpp
    VariableWidth.hpp
    Zipper.cpp
    Zipper.hpp
    FilamentGroup.hpp
    FilamentGroup.cpp
    FilamentGroupUtils.hpp
    FilamentGroupUtils.cpp
    GCode/ToolOrderUtils.hpp
    GCode/ToolOrderUtils.cpp
    FlushVolPredictor.hpp
    FlushVolPredictor.cpp
)

if (APPLE)
    list(APPEND lisbslic3r_sources
            MacUtils.mm
            Format/ModelIO.hpp
            Format/ModelIO.mm
        )
endif ()

add_library(libslic3r STATIC ${lisbslic3r_sources} )
# qhullcpp is PUBLIC: static libslic3r objects reference orgQhull::Qhull (TriangleMesh::its_convex_hull); consumers must link qhull.
target_link_libraries(libslic3r PRIVATE Boost::filesystem Boost::system Boost::log CGAL::CGAL mcut)
target_link_libraries(libslic3r PUBLIC qhullcpp clipper Clipper2 glu-libtess)
# PUBLIC: TBB + Nowide symbols used from libslic3r objects must reach the final link (static lib).
# Boost::iostreams PUBLIC: DRC.cpp uses mapped_file_source (Format/DRC.cpp); symbols must reach app link.
target_link_libraries(libslic3r PUBLIC TBB::tbb TBB::tbbmalloc Boost::nowide Boost::locale Boost::iostreams)
target_link_libraries(libslic3r PUBLIC JPEG::JPEG draco::draco EXPAT::expat png_static noise::noise)

if(WIN32)
  # Boost.UUID random_provider uses BCryptGenRandom (Windows CNG).
  target_link_libraries(libslic3r PUBLIC bcrypt)
endif()

target_compile_definitions(libslic3r PRIVATE
    UNICODE
    _UNICODE
    EIGEN_HAS_STD_RESULT_OF=0
)


target_compile_definitions(libslic3r PRIVATE _USE_MATH_DEFINES)
#target_precompile_headers(libslic3r PRIVATE pchheader.hpp)
target_compile_definitions(libslic3r PRIVATE CEREAL_THREAD_SAFE=1)
add_precompiled_header(libslic3r pchheader.hpp FORCEINCLUDE)
target_compile_options(libslic3r PRIVATE /Zc:char8_t-)
target_compile_definitions(libslic3r PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(libslic3r PRIVATE /bigobj)
if(MSVC)
  # MSVC /Zc:ternary (default in VS2019+) rejects some CGAL ?: involving Lazy_exact_nt<gmp_rational> inside templates (中文报错「条件表达式无效」).
  target_compile_options(libslic3r PRIVATE /Zc:ternary-)
  # Parallel builds: many cl.exe writing one libslic3r.pdb can still hit C1041 even with /FS (AV locks, MSBuild, etc.).
  # /Z7 embeds debug info in .obj files so compilation does not share a PDB (linker still produces .pdb for the static lib if needed).
  target_compile_options(libslic3r PRIVATE /FS)
  target_compile_options(libslic3r PRIVATE $<$<CONFIG:Debug>:/Z7> $<$<CONFIG:RelWithDebInfo>:/Z7>)
endif()
# source_group(TREE) requires every file to be under the root; vendored C sources live under src/third_party.
set(_libslic3r_source_group_files ${lisbslic3r_sources})
list(FILTER _libslic3r_source_group_files EXCLUDE REGEX "^\\.\\./third_party/")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_libslic3r_source_group_files})
source_group("third_party/semver" FILES ${CMAKE_CURRENT_LIST_DIR}/../third_party/semver/semver.c)
source_group("third_party/nanosvg" FILES ${CMAKE_CURRENT_LIST_DIR}/../third_party/nanosvg/nanosvg.c)
source_group("third_party/miniz" FILES ${CMAKE_CURRENT_LIST_DIR}/../third_party/miniz/miniz.c)
source_group("third_party/qoi" FILES ${CMAKE_CURRENT_LIST_DIR}/../third_party/qoi/qoi.c)
