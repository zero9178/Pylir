
#pragma once

#include <pylir/Support/BigInt.hpp>
#include <pylir/Support/HashTable.hpp>

#include <array>
#include <string_view>
#include <type_traits>

#include <unwind.h>

namespace pylir::rt
{

class PyTypeObject;
class PySequence;
class PyDict;
class PyFunction;

class PyObject
{
    PyTypeObject* m_type;

public:
    PyObject(PyTypeObject* type) : m_type(type) {}

    PyObject(const PyObject&) = delete;
    PyObject(PyObject&&) noexcept = delete;
    PyObject& operator=(const PyObject&) = delete;
    PyObject& operator=(PyObject&&) noexcept = delete;

    PyObject* getType()
    {
        return reinterpret_cast<PyObject*>(m_type);
    }

    template <class T>
    bool isa();

    template <class T>
    T* cast()
    {
        // TODO: static_assert(std::is_pointer_interconvertible_base_of_v<PyObject, T>);
        return reinterpret_cast<T*>(this);
    }

    template <class T>
    T* dyn_cast()
    {
        return isa<T>() ? cast<T>() : nullptr;
    }

    PyObject* getSlot(int index);

    PyObject* getSlot(std::string_view name);

    PyObject* call(PySequence* args, PyDict* keywords);

    PyObject* call(std::initializer_list<PyObject*> args);

    bool isInstanceOf(pylir::rt::PyObject* typeObject);
};

static_assert(std::is_standard_layout_v<PyObject>);

namespace Builtin
{
#define BUILTIN(name, symbol, ...) extern PyObject name asm(symbol);
#include <pylir/Interfaces/Builtins.def>
} // namespace Builtin

using PyUniversalCC = PyObject* (*)(PyFunction*, PySequence*, PyDict*);

class PyTypeObject
{
    PyObject m_base;
    std::size_t m_offset;

public:
    std::size_t getOffset()
    {
        return m_offset;
    }

    enum Slots
    {
#define TYPE_SLOT(x, ...) x,
#include <pylir/Interfaces/Slots.def>
    };

    PySequence* getMRO()
    {
        return reinterpret_cast<pylir::rt::PySequence*>(m_base.getSlot(Slots::__mro__));
    }
};

static_assert(std::is_standard_layout_v<PyTypeObject>);

class PyFunction
{
    PyObject m_base;
    PyUniversalCC m_function;

public:
    enum Slots
    {
#define FUNCTION_SLOT(x, ...) x,
#include <pylir/Interfaces/Slots.def>
    };

    PyObject* call(PySequence* args, PyDict* keywords)
    {
        return m_function(this, args, keywords);
    }
};

static_assert(std::is_standard_layout_v<PyFunction>);

template <class T>
struct BufferComponent
{
    std::size_t size{};
    std::size_t capacity{};
    T* array{};
};

class PySequence
{
    PyObject m_base;
    BufferComponent<PyObject*> m_buffer;

protected:
    PySequence(PyTypeObject* type, BufferComponent<PyObject*> data) : m_base(type), m_buffer(data) {}

public:
    PyObject** begin()
    {
        return m_buffer.array;
    }

    PyObject** end()
    {
        return m_buffer.array + m_buffer.size;
    }

    std::size_t len() const
    {
        return m_buffer.size;
    }

    PyObject* getItem(std::size_t index)
    {
        return m_buffer.array[index];
    }
};

static_assert(std::is_standard_layout_v<PySequence>);

class PyString
{
    PyObject m_base;
    BufferComponent<char> m_buffer;

public:
    friend bool operator==(PyString& lhs, std::string_view sv)
    {
        return lhs.view() == sv;
    }

    friend bool operator==(const std::string_view sv, PyString& rhs)
    {
        return rhs.view() == sv;
    }

    std::string_view view() const
    {
        return std::string_view{m_buffer.array, m_buffer.size};
    }

    std::size_t len() const
    {
        return m_buffer.size;
    }
};

static_assert(std::is_standard_layout_v<PySequence>);

struct PyObjectHasher
{
    std::size_t operator()(PyObject* object) const noexcept;
};

struct PyObjectEqual
{
    bool operator()(PyObject* lhs, PyObject* rhs) const noexcept;
};

class PyDict
{
    PyObject m_base;
    HashTable<PyObject*, PyObject*, PyObjectHasher, PyObjectEqual> m_table;

public:
    PyDict() : m_base(reinterpret_cast<PyTypeObject*>(&Builtin::Dict)) {}

    PyObject* tryGetItem(PyObject* key);

    void setItem(PyObject* key, PyObject* value)
    {
        m_table.insert_or_assign(key, value);
    }

    void delItem(PyObject* key)
    {
        m_table.erase(key);
    }
};

static_assert(std::is_standard_layout_v<PyDict>);

class PyInt
{
    PyObject m_base;
    BigInt m_integer;

public:
    bool boolean()
    {
        return !m_integer.isZero();
    }

    template <class T>
    T to()
    {
        return m_integer.getInteger<T>();
    }
};

static_assert(std::is_standard_layout_v<PyInt>);

class PyBaseException
{
    PyObject m_base;
    std::uintptr_t m_landingPad;
    _Unwind_Exception m_unwindHeader;
    std::uint32_t m_typeIndex;

public:
    constexpr static std::uint64_t EXCEPTION_CLASS = 0x50594C5250590000; // PYLRPY\0\0

    enum Slots
    {
#define BASEEXCEPTION_SLOT(x, ...) x,
#include <pylir/Interfaces/Slots.def>
    };

    _Unwind_Exception& getUnwindHeader()
    {
        static_assert(offsetof(PyBaseException, m_unwindHeader) == alignof(_Unwind_Exception));
        return m_unwindHeader;
    }

    static PyBaseException* fromUnwindHeader(_Unwind_Exception* header)
    {
        PYLIR_ASSERT(header->exception_class == EXCEPTION_CLASS);
        return reinterpret_cast<PyBaseException*>(reinterpret_cast<char*>(header)
                                                  - offsetof(PyBaseException, m_unwindHeader));
    }

    std::uintptr_t getLandingPad() const
    {
        return m_landingPad;
    }

    void setLandingPad(std::uintptr_t landingPad)
    {
        m_landingPad = landingPad;
    }

    std::uint32_t getTypeIndex() const
    {
        return m_typeIndex;
    }

    void setTypeIndex(std::uint32_t typeIndex)
    {
        m_typeIndex = typeIndex;
    }
};

static_assert(std::is_standard_layout_v<PyBaseException>);

template <class T>
inline bool PyObject::isa()
{
    static_assert(sizeof(T) && false, "No specialization available");
    PYLIR_UNREACHABLE;
}

template <>
inline bool PyObject::isa<PyTypeObject>()
{
    return isInstanceOf(&Builtin::Type);
}

template <>
inline bool PyObject::isa<PySequence>()
{
    return isInstanceOf(&Builtin::Tuple) || isInstanceOf(&Builtin::List);
}

template <>
inline bool PyObject::isa<PyDict>()
{
    return isInstanceOf(&Builtin::Dict);
}

template <>
inline bool PyObject::isa<PyFunction>()
{
    return isInstanceOf(&Builtin::Function);
}

template <>
inline bool PyObject::isa<PyString>()
{
    return isInstanceOf(&Builtin::Str);
}

template <>
inline bool PyObject::isa<PyInt>()
{
    return isInstanceOf(&Builtin::Int);
}

} // namespace pylir::rt
