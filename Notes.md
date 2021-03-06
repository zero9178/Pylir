# Getting & Setting Attributes

## Getter:

* Lookup `__getattribute__` in type
    - Goes through MRO
    - If found -> execute and return
* Lookup attribute in type
    - Goes through MRO
    - If found && has `__get__` && `__set__` methods -> execute `__get__` and return
* Lookup in `__dict__` if available
* if type lookup result was successful && had `__get__` but not `__set__` -> execute `__get__` and return
* if type lookup result was successful -> return it
* Lookup `__getattr__` in type
    - Goes through MRO
    - If found -> execute and return Else:
      raise AttributeError

Worst case unoptimized lookup count: len(MRO) (`__getattribute__`) + 1 (`__dict__`) + len(MRO) + (`__getattr__`)

Lookup for `__getattribute__` and `__getattr__` could potentially be combined making it len(MRO) + 1.

Would improve the worst case but potentially worsen the best case

Setter:

* Lookup attribute in type
    - Goes through MRO
    - If found && has `__set__` methods -> execute `__set__` and return
* Set in `__dict__` if available Else:
  raise AttributeError

# Method lookup

* Lookup in type
    - Goes through MRO
    - if found && has `__get__` -> execute and return
    - if found return result Else:
        - raise AttributeError or TypeError

# Attempting a call

Uses method lookup for `__call__` until a function has been found.

Recursive version: akin to what CPython uses

```python
def call(self, *args, **kwargs):
    if not hasattr(self, '__call__'):
        raise TypeError
    callable = self.__call__
    if callable is function:
        return (callable.fptr)(callable, self, *args, **kwargs)
    if hasattr(callable, '__get__'):
        callable = call(callable.__get__, self, type(self))
    return call(callable, *args, **kwargs)
```

Mostly iterative version

```python
def call(self, *args, **kwd):
    while hasattr(self, '__call__'):
        callable = self.__call__
        if callable is function:
            return (callable.fptr)(callable, self, *args, **kwd)
        if hasattr(callable, '__get__'):
            callable = call(callable.__get__, self, type(self))
        self = callable
    raise TypeError
```

# Creating a class

Given `*args` and `**kw` from the argument list of a class definition:

* Get `bases` by iterating over `args`
    * If the element is a subtype of `type`: add to the list of bases
    * Else: Lookup `__mro_entries__` method via normal attribute lookup (not method lookup) and call it with `*args`
        * If lookup fails or `__mro_entries__` does not return a tuple error
        * Otherwise replace current `arg` with tuple entries
* Get metatype by looking up `metaclass` in `**kw`
    * If unsuccessful: Make `metaclass` the type of the first arg in `bases` or `type` if empty
    * If successful: Remove `metaclass` from `**kw`
* If `metaclass` is a subclass of `type`:
    * Calculate `metaclass` by going through all bases and checking:
        * If type of `base` is subclass of `metaclass` continue
        * If `metaclass` is subclass of type of `base`: Set `metaclass` to type of `base`
        * Else: TypeError
* If `metaclass` has `__prepare__` found via attribute lookup:
    * call it to get a mapping for `namespace`
    * Else: assign an empty dict to `namespace`
* Execute body of the class with `namespace` as namespace
* Execute `metaclass(name, bases, namespace, **kw)`

Issue: Calling `metaclass` in the most common case of `metaclass is type` will result in the 3 argument version of
`type` being called which actually does the EXACT same procedure as the above. Since `metaclass` is not guaranteed
to be `type` however we can't not do the above. Since `type` might also manually be called by the user we can't not do
the above in `type` either. We have no way to know whether `metaclass` is actually `type` unless going through the above
as well. The best way is likely to simply call an internal function which is also called by `type`s call operator
that does all the dynamic type creation.

# Calling a reversible binary operator

Given 'lhs' and 'rhs', which are the left and right operand respectively:

* if typeOf rhs subtype of typeOf lhs but not the same type:
    * Check rhsType implements reflected operator, if it doesn't break
    * Check lhsType either doesn't implement reflected operator or has a different implementation, else break
    * Call reflected operator of rhs with (rhs, lhs)
    * If result is not NotImplemented return value, else continue
* Call normal operator of lhs with (lhs, rhs)
* If result is not NotImplemented return
* If rhs and lhs are of the same type raise TypeError
* If reverse operator has not been tried yet try it
* If not found or NotImplemented is returned, raise TypeError

```py
def bin_op(normal, reverse, lhs, rhs):
    tried_reverse = False
    if type(lhs) is not type(rhs) and issubclass(type(rhs), type(lhs)):
        rhs_impl = type(rhs).mroLookup(reverse)
        if rhs_impl is not None:
            lhs_impl = type(lhs).mroLookup(reverse)
            if lhs_impl is None or lhs_impl is not rhs_impl:
                tried_reverse = True
                result = rhs_impl(rhs, lhs)
                if result is not NotImplemented:
                    return result
    result = type(lhs).mroLookup(normal)(lhs, rhs)
    if result is not NotImplemented:
        return result
    if type(lhs) is type(rhs):
        raise TypeError
    if not tried_reverse:
        result = type(rhs).mroLookup(reverse)(rhs, lhs)
    if result is NotImplemented:
        raise TypeError
    return result
```
