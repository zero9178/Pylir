// RUN: pylir-opt %s --pylir-monomorph --split-input-file | FileCheck %s

py.globalValue const @builtins.type = #py.type<slots = {__slots__ = #py.tuple<(#py.str<"__add__">)>}>
py.globalValue const @builtins.tuple = #py.type
py.globalValue const @builtins.str = #py.type
py.globalValue const @builtins.dict = #py.type
py.globalValue const @builtins.function = #py.type
py.globalValue const @builtins.None = #py.type

func @builtins.int.__add__$impl(%arg0: !py.dynamic, %arg1: !py.dynamic, %arg2: !py.dynamic) -> !py.dynamic {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %0 = py.tuple.getItem %arg1[%c0]
  %1 = py.tuple.getItem %arg1[%c1]
  %2 = py.int.add %0, %1
  return %2 : !py.dynamic
}

py.globalValue @builtins.int.__add__ = #py.function<@builtins.int.__add__$impl>
py.globalValue const @builtins.int = #py.type<mroTuple = #py.tuple<(@builtins.int)>, slots = {__add__ = @builtins.int.__add__}>

func @__init__() {
  %0 = py.constant(@builtins.int)
  %1 = py.constant(#py.int<1>)
  %2 = py.constant(#py.int<0>)
  cf.br ^bb1(%2 : !py.dynamic)

^bb1(%3: !py.dynamic):  // 2 preds: ^bb0, ^bb1
  %4 = py.type.mro %0
  %result, %success = py.mroLookup "__add__" in %4
  %5 = py.makeTuple (%3, %1)
  %6 = py.constant(#py.dict<{}>)
  %7 = py.call @builtins.int.__add__$impl_0(%result, %5, %6) : (!py.dynamic, !py.dynamic, !py.dynamic) -> !py.dynamic
  %8 = test.random
  cf.cond_br %8, ^bb1(%7 : !py.dynamic), ^bb2

^bb2:  // pred: ^bb1
  test.use(%7) : !py.dynamic
  py.unreachable
}

func private @builtins.int.__add__$impl_0(%arg0: !py.dynamic {py.specialization_args = !py.unknown}, %arg1: !py.dynamic {py.specialization_args = !py.tuple<(!py.class<@builtins.int>, !py.class<@builtins.int>)>}, %arg2: !py.dynamic {py.specialization_args = !py.class<@builtins.dict>}) -> (!py.dynamic {py.specialization_args = !py.class<@builtins.int>}) attributes {py.specialization_of = "builtins.int.__add__$impl"} {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %0 = py.tuple.getItem %arg1[%c0]
  %1 = py.tuple.getItem %arg1[%c1]
  %2 = py.int.add %0, %1
  return %2 : !py.dynamic
}

// CHECK-LABEL: func @builtins.int.__add__$impl
// CHECK-LABEL: func @__init__
// CHECK-LABEL: func private @builtins.int.__add__$impl_0
// CHECK-NOT: func {{.*}} @{{.*}}
