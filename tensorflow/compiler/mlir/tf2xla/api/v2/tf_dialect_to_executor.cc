/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tf2xla/api/v2/tf_dialect_to_executor.h"

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/compiler/jit/flags.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/data_dumper_logger_config.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/dump_mlir_util.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/util/debug_data_dumper.h"
#include "tsl/platform/status.h"

namespace tensorflow {
namespace tf2xla {
namespace v2 {

using mlir::LogicalResult;
using mlir::ModuleOp;
using mlir::OpPassManager;
using mlir::PassManager;
using mlir::func::FuncOp;

namespace {

// Add logger to bridge passmanager.
// Enable timing statistics per pass for the bridge passmanager.
void EnableDetailedLogging(PassManager *pm,
                           llvm::StringRef module_name = llvm::StringRef()) {
  // Print the whole module after each pass, which requires disabling
  // multi-threading as well.
  pm->getContext()->disableMultithreading();
  pm->enableIRPrinting(std::make_unique<::tensorflow::DataDumperLoggerConfig>(
      [module_name](const std::string &pass_tag_name, mlir::Operation *op) {
        return DEBUG_DATA_DUMPER()->GetDumpFilename(
            module_name.str(), kDebugGroupBridgePhase1, pass_tag_name);
      },
      "",
      /*print_module_scope=*/true));
  pm->enableTiming();
}

void AddGraphExportLoweringPasses(OpPassManager &pm) {
  pm.addPass(mlir::TF::CreateTFRegionControlFlowToFunctional());

  // First, we need to convert from functional, to executor dialect.
  pm.addNestedPass<FuncOp>(
      mlir::CreateFunctionalToExecutorDialectConversionPass());

  // Do a single pass to split the graph's single island op into an island per
  // op as expected by the following passes.
  pm.addNestedPass<FuncOp>(mlir::TF::CreateSplitIntoIslandPerOpPass());

  pm.addNestedPass<FuncOp>(mlir::TFDevice::CreateReplicateToIslandPass(
      /*legacy_graph_export=*/false));
  pm.addNestedPass<FuncOp>(
      mlir::TFDevice::CreateReplicaIDToDeviceOrdinalPass());
  pm.addNestedPass<FuncOp>(mlir::TFDevice::CreateParallelExecuteToIslandsPass(
      /*legacy_graph_export=*/false));
  pm.addNestedPass<FuncOp>(mlir::TFDevice::CreateLaunchToDeviceAttributePass(
      /*legacy_graph_export=*/false));

  // Do a single pass to encode necessary control deps in the IR according to
  // the results of side effect analysis.
  pm.addPass(
      mlir::tf_executor::CreateTFExecutorUpdateControlDependenciesPass());

  pm.addNestedPass<FuncOp>(mlir::TFTPU::CreateTPUDevicePropagationPass());
  pm.addNestedPass<FuncOp>(mlir::TFTPU::CreateTPUColocateSplitsPass());
  pm.addPass(mlir::createSymbolDCEPass());
  if (tensorflow::GetMlirCommonFlags()
          ->tf_mlir_enable_convert_control_to_data_outputs_pass) {
    pm.addPass(
        mlir::tf_executor::CreateTFExecutorConvertControlToDataOutputsPass());
  }
  pm.addPass(mlir::TF::CreateVerifySuitableForExportPass());
}

}  // namespace

tensorflow::Status ExportFromTensorflowDialectToExecutor(
    ModuleOp module, llvm::StringRef module_name) {
  PassManager tf_to_executor(module.getContext());
  ::tensorflow::applyTensorflowAndCLOptions(tf_to_executor);
  AddGraphExportLoweringPasses(tf_to_executor);

  if (VLOG_IS_ON(1) ||
      DEBUG_DATA_DUMPER()->ShouldDump(module_name.str(), kDebugGroupMain)) {
    ::tensorflow::DumpMlirOpToFile(
        DEBUG_DATA_DUMPER()->GetDumpFilename(
            module_name.str(), kDebugGroupMain,
            "tfxla_bridge_tfdialect_to_executor_before"),
        module, llvm::StringRef(), &tf_to_executor);

    if (VLOG_IS_ON(2) || DEBUG_DATA_DUMPER()->ShouldDump(
                             module_name.str(), kDebugGroupBridgePhase1)) {
      EnableDetailedLogging(&tf_to_executor, module_name);
    }
  }

  LogicalResult result = tf_to_executor.run(module);

  if (VLOG_IS_ON(1) ||
      DEBUG_DATA_DUMPER()->ShouldDump(module_name.str(), kDebugGroupMain)) {
    ::tensorflow::DumpMlirOpToFile(
        DEBUG_DATA_DUMPER()->GetDumpFilename(
            module_name.str(), kDebugGroupMain,
            "tfxla_bridge_tfdialect_to_executor_after"),
        module, llvm::StringRef(), &tf_to_executor);
  }

  if (result.succeeded()) {
    return tsl::OkStatus();
  }

  return absl::InternalError(
      "Failed to export from TF Dialect to TF Executor Dialect.");
}

}  // namespace v2
}  // namespace tf2xla
}  // namespace tensorflow
