// RUN: pylir-opt %s -convert-pylir-to-llvm --split-input-file | FileCheck %s

py.globalValue const @builtins.type = #py.type<slots = {__slots__ = #py.tuple<(#py.str<"__slots__">,#py.str<"__eq__">,#py.str<"__hash__">)>}>

py.globalValue const @builtins.object = #py.type // stub
py.globalValue const @builtins.str = #py.type // stub
py.globalValue const @builtins.tuple = #py.type // stub

// CHECK-LABEL: @builtins.type
// CHECK-NEXT: %[[UNDEF:.*]] = llvm.mlir.undef
// CHECK-NEXT: %[[TYPE:.*]] = llvm.mlir.addressof @builtins.type
// CHECK-NEXT: %[[UNDEF1:.*]] = llvm.insertvalue %[[TYPE]], %[[UNDEF]][0 : i32]
// CHECK-NEXT: %[[AS_COUNT:.*]] = llvm.mlir.constant
// CHECK-NEXT: %[[UNDEF2:.*]] = llvm.insertvalue %[[AS_COUNT]], %[[UNDEF1]][1 : i32]
// CHECK-NEXT: %[[LAYOUT:.*]] = llvm.mlir.addressof @builtins.type
// CHECK-NEXT: %[[UNDEF3:.*]] = llvm.insertvalue %[[LAYOUT]], %[[UNDEF2]][2 : i32]
// CHECK-NEXT: %[[MRO:.*]] = llvm.mlir.addressof
// CHECK-NEXT: %[[UNDEF4:.*]] = llvm.insertvalue %[[MRO]], %[[UNDEF3]][3 : i32]
// CHECK-NEXT: %[[ADDRESS:.*]] = llvm.mlir.addressof
// CHECK-NEXT: %[[UNDEF5:.*]] = llvm.insertvalue %[[ADDRESS]], %[[UNDEF4]][4 : i32, 0 : i32]
// CHECK-NEXT: %[[NULL:.*]] = llvm.mlir.null
// CHECK-NEXT: %[[UNDEF6:.*]] = llvm.insertvalue %[[NULL]], %[[UNDEF5]][4 : i32, 1 : i32]
// CHECK-NEXT: %[[NULL:.*]] = llvm.mlir.null
// CHECK-NEXT: %[[UNDEF7:.*]] = llvm.insertvalue %[[NULL]], %[[UNDEF6]][4 : i32, 2 : i32]
// CHECK-NEXT: llvm.return %[[UNDEF7]]
