// RUN: pylir-opt %s --pylir-monomorph --split-input-file | FileCheck %s

py.globalValue const @builtins.type = #py.type<slots = {__slots__ = #py.tuple<(#py.str<"__add__">)>}>
py.globalValue const @builtins.tuple = #py.type
py.globalValue const @builtins.str = #py.type
py.globalValue const @builtins.dict = #py.type
py.globalValue const @builtins.function = #py.type
py.globalValue const @builtins.None = #py.type

func @builtins.int.__add__$impl(%closure : !py.dynamic, %tuple : !py.dynamic, %dict : !py.dynamic) -> !py.dynamic {
	%zero = arith.constant 0 : index
	%one = arith.constant 1 : index
	%first = py.tuple.getItem %tuple[%zero]
	%second = py.tuple.getItem %tuple[%one]
	%result = py.int.add %first, %second
	return %result : !py.dynamic
}

py.globalValue @builtins.int.__add__ = #py.function<@builtins.int.__add__$impl>
py.globalValue const @builtins.int = #py.type<slots = {__add__ = @builtins.int.__add__}, mroTuple = #py.tuple<(@builtins.int)>>

func @__init__() {
	%one = py.constant(#py.int<1>)
	%zero = py.constant(#py.int<0>)
	cf.br ^loop(%zero : !py.dynamic)

^loop(%iter : !py.dynamic):
	%0 = py.typeOf %iter
	%1 = py.type.mro %0
	%2, %found = py.mroLookup "__add__" in %1
	%3 = py.makeTuple (%iter, %one)
	%4 = py.constant(#py.dict<{}>)
	%5 = py.function.call %2(%2, %3, %4)
	%6 = test.random
	cf.cond_br %6, ^loop(%5 : !py.dynamic), ^exit

^exit:
	test.use(%5) : !py.dynamic
	py.unreachable
}

// CHECK-LABEL: func @__init__
// CHECK-DAG: %[[ONE:.*]] = py.constant(#py.int<1>)
// CHECK-DAG: %[[ZERO:.*]] = py.constant(#py.int<0>)
// CHECK-DAG: %[[INT_TYPE:.*]] = py.constant(@builtins.int)
// CHECK: cf.br ^[[LOOP:.*]](%[[ZERO]] : {{.*}})
// CHECK-NEXT: ^[[LOOP]]
// CHECK-SAME: %[[ITER:[[:alnum:]]+]]
// CHECK-NEXT: %[[MRO:.*]] = py.type.mro %[[INT_TYPE]]
// CHECK-NEXT: %[[LOOKUP:.*]], %{{.*}} = py.mroLookup "__add__" in %[[MRO]]
// CHECK-NEXT: %[[TUPLE:.*]] = py.makeTuple (%[[ITER]], %[[ONE]])
// CHECK-NEXT: %[[DICT:.*]] = py.constant(#py.dict<{}>)
// CHECK-NEXT: %[[RESULT:.*]] = py.call @[[CLONED:.*]](%[[LOOKUP]], %[[TUPLE]], %[[DICT]])
// CHECK: cf.cond_br %{{.*}}, ^[[LOOP]](%[[RESULT]] : {{.*}}), ^[[EXIT:[[:alnum:]]+]]
// CHECK-NEXT: ^[[EXIT]]:
// CHECK-NEXT: test.use(%[[RESULT]])

// CHECK: func private @[[CLONED]]
// CHECK-SAME: %{{[[:alnum:]]+}}
// CHECK-SAME: %[[TUPLE:[[:alnum:]]+]]
// CHECK-SAME: %{{[[:alnum:]]+}}
// CHECK-NEXT: %[[ZERO:.*]] = arith.constant 0
// CHECK-NEXT: %[[ONE:.*]] = arith.constant 1
// CHECK-NEXT: %[[FIRST:.*]] = py.tuple.getItem %[[TUPLE]][%[[ZERO]]]
// CHECK-NEXT: %[[SECOND:.*]] = py.tuple.getItem %[[TUPLE]][%[[ONE]]]
// CHECK-NEXT: %[[RESULT:.*]] = py.int.add %[[FIRST]], %[[SECOND]]
// CHECK-NEXT: return %[[RESULT]]
