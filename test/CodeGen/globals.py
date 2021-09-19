# RUN: pylir %s -emit-mlir -o - | FileCheck %s

# CHECK-DAG: global @x
# CHECK-DAG: global @y
# CHECK-DAG: global @z
# CHECK-DAG: global @foo

x = 2

# CHECK-DAG: %[[VALUE:.*]] = py.constant #py.int<2>
# CHECK-DAG: %[[X:.*]] = py.getGlobal @x
# CHECK: py.store %[[VALUE]] into %[[X]]

x


# CHECK: %[[X:.*]] = py.getGlobal @x
# CHECK: py.load %[[X]]

def foo():
    global y
    y = 3


(z := 3)

# CHECK-DAG: %[[VALUE:.*]] = py.constant #py.int<3>
# CHECK-DAG: %[[Z:.*]] = py.getGlobal @z
# CHECK: py.store %[[VALUE]] into %[[Z]]

# CHECK-LABEL: func private @"foo$impl[0]"
# CHECK: %[[VALUE:.*]] = py.constant #py.int<3>
# CHECK: %[[Y:.*]] = py.getGlobal @y
# CHECK: py.store %[[VALUE]] into %[[Y]]
