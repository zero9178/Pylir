// RUN: pylir-opt %s -canonicalize | FileCheck %s

py.globalValue @builtins.type = #py.type
py.globalValue @builtins.int = #py.type

func.func @test1(%arg0 : !py.dynamic) -> !py.dynamic {
    %0 = py.constant(#py.int<5>)
    %1 = py.constant(#py.int<3>)
    %2 = py.int.add %arg0, %0
    %3 = py.int.add %2, %1
    return %3 : !py.dynamic
}

// CHECK-LABEL: @test1
// CHECK-SAME: %[[ARG0:[[:alnum:]]+]]
// CHECK-NEXT: %[[C:.*]] = py.constant(#py.int<8>)
// CHECK-NEXT: %[[RESULT:.*]] = py.int.add %[[ARG0]], %[[C]]
// CHECK-NEXT: return %[[RESULT]]

func.func @test2(%arg0 : !py.dynamic, %arg1 : !py.dynamic) -> !py.dynamic {
    %0 = py.constant(#py.int<5>)
    %1 = py.constant(#py.int<3>)
    %2 = py.int.add %arg0, %0
    %3 = py.int.add %arg1, %1
    %4 = py.int.add %2, %3
    return %4 : !py.dynamic
}

// CHECK-LABEL: @test2
// CHECK-SAME: %[[ARG0:[[:alnum:]]+]]
// CHECK-SAME: %[[ARG1:[[:alnum:]]+]]
// CHECK-NEXT: %[[C:.*]] = py.constant(#py.int<8>)
// CHECK-NEXT: %[[SUM:.*]] = py.int.add %[[ARG0]], %[[ARG1]]
// CHECK-NEXT: %[[RESULT:.*]] = py.int.add %[[SUM]], %[[C]]
// CHECK-NEXT: return %[[RESULT]]
