#ifndef GRANITA_H
#define GRANITA_H
#include "ruby.h"

// for explanation of WTF is happening here, see ruby.h and
// https://silverhammermba.github.io/emberb/c/ and
// http://clalance.blogspot.com/2011/01/writing-ruby-extensions-in-c-part-9.html

// ****
// ****                       Singletons
// ****

VALUE sorbet_rubyTrue() {
    return RUBY_Qtrue;
}

VALUE sorbet_rubyFalse() {
    return RUBY_Qfalse;
}

VALUE sorbet_rubyNil() {
    return RUBY_Qnil;
}

// ****
// ****                       Conversions between Ruby values and C values
// ****
long sorbet_rubyValueToLong(VALUE val) {
    return FIX2LONG(val);
}

VALUE sorbet_longToRubyValue(long i) {
    return LONG2FIX(i);
}

double sorbet_rubyValueToDouble(VALUE val) {
    return RFLOAT_VALUE(val);
}

VALUE sorbet_doubleToRubyValue(double u) {
    return DBL2NUM(u);
}

// ****
// ****                       Integer
// ****
VALUE sorbet_Integer_plus_Integer(VALUE a, VALUE b) {
    return sorbet_longToRubyValue(sorbet_rubyValueToLong(a) + sorbet_rubyValueToLong(b));
}

VALUE sorbet_Integer_minus_Integer(VALUE a, VALUE b) {
    return sorbet_longToRubyValue(sorbet_rubyValueToLong(a) - sorbet_rubyValueToLong(b));
}

VALUE sorbet_Integer_less_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) < sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_greater_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) > sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_greatereq_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) >= sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_lesseq_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) <= sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_eq_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) == sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

VALUE sorbet_Integer_neq_Integer(VALUE a, VALUE b) {
    return (sorbet_rubyValueToLong(a) != sorbet_rubyValueToLong(b)) ? RUBY_Qtrue : RUBY_Qfalse;
}

// ****
// ****                       Operations on Strings
// ****
const char *sorbet_rubyStringToCPtr(VALUE value) {
    return RSTRING_PTR(value);
}

long sorbet_rubyStringLength(VALUE value) {
    return RSTRING_LEN(value);
}

VALUE sorbet_CPtrToRubyString(const char *ptr, long length) {
    return rb_str_new(ptr, length);
}

VALUE sorbet_stringPlus(VALUE str1, VALUE str2) {
    return rb_str_plus(str1, str2);
}

// ****
// ****                       Operations on Arrays
// ****
long sorbet_rubyArrayLen(VALUE array) {
    return RARRAY_LEN(array);
}

VALUE sorbet_newRubyArray() {
    return rb_ary_new();
}

VALUE sorbet_newRubyArrayWithElems(long size, const VALUE *elems) {
    return rb_ary_new4(size, elems);
}

void sorbet_arrayPush(VALUE array, VALUE element) {
    rb_ary_push(array, element);
}

void sorbet_arrayStore(VALUE array, long idx, VALUE value) {
    rb_ary_store(array, idx, value);
}

VALUE sorbet_arrayGet(VALUE array, long idx) {
    return rb_ary_entry(array, idx);
}

// ****
// ****                       Operations on Hashes
// ****
//
VALUE sorbet_newRubyHash() {
    return rb_hash_new();
}

void sorbet_hashStore(VALUE hash, VALUE key, VALUE value) {
    rb_hash_aset(hash, key, value);
}

VALUE sorbet_hashGet(VALUE hash, VALUE key) {
    return rb_hash_aref(hash, key);
}

// possible return values for `func`:
//  - ST_CONTINUE, then the rest of the hash is processed as normal.
//  - ST_STOP, then no further processing of the hash is done.
//  - ST_DELETE, then the current hash key is deleted from the hash and the rest
//  of the hash is processed
//  - ST_CHECK, then the hash is checked to see if it has been modified during
//  this operation. If so, processing of the hash stops.
/*
void sorbet_hashEach(VALUE hash, int(*func)(VALUE key, VALUE val,
VALUE in), VALUE closure) { return rb_hash_foreach(hash, func, closure);
}
*/

// ****
// ****                       Operations on Ruby ID's
// ****

ID sorbet_IDIntern(const char *value) {
    return rb_intern(value);
}

ID sorbet_symToID(VALUE sym) {
    return SYM2ID(sym);
}

ID sorbet_IDToSym(ID id) {
    return ID2SYM(id);
}

VALUE sobet_getRubyClassOf(VALUE value) {
    return CLASS_OF(value);
}

const char *sorbet_getRubyClassName(VALUE object) {
    return rb_obj_classname(object);
}
// ****
// ****                       Tests
// ****

_Bool sorbet_testIsTruthy(VALUE value) {
    return RB_TEST(value);
}

_Bool sorbet_testIsTrue(VALUE value) {
    return value == RUBY_Qtrue;
}

_Bool sorbet_testIsFalse(VALUE value) {
    return value == RUBY_Qfalse;
}

_Bool sorbet_testIsNil(VALUE value) {
    return value == RUBY_Qnil;
}

_Bool sorbet_testIsUndef(VALUE value) {
    return value == RUBY_Qundef;
}

_Bool sorbet_testIsSymbol(VALUE value) {
    return RB_SYMBOL_P(value);
}

_Bool sorbet_testIsFloat(VALUE value) {
    return RB_FLOAT_TYPE_P(value);
}

_Bool sorbet_testIsHash(VALUE value) {
    return TYPE(value) == RUBY_T_HASH;
}

_Bool sorbet_testIsArray(VALUE value) {
    return TYPE(value) == RUBY_T_ARRAY;
}

_Bool sorbet_testIsString(VALUE value) {
    return TYPE(value) == RUBY_T_STRING;
}

// ****
// ****                       Variables
// ****

VALUE sorbet_instanceVariableGet(VALUE receiver, ID name) {
    return rb_ivar_get(receiver, name);
}

VALUE sorbet_instanceVariableSet(VALUE receiver, ID name, VALUE newValue) {
    return rb_ivar_set(receiver, name, newValue);
}

VALUE sorbet_classVariableGet(VALUE _class, ID name) {
    return rb_cvar_get(_class, name);
}

void sorbet_classVariableSet(VALUE _class, ID name, VALUE newValue) {
    rb_cvar_set(_class, name, newValue);
}

// ****
// ****                       Constants, Classes and Modules
// ****

void sorbet_defineTopLevelConstant(const char *name, VALUE value) {
    rb_define_global_const(name, value);
}

void sorbet_defineNestedCosntant(VALUE owner, const char *name, VALUE value) {
    rb_define_const(owner, name, value);
}

// DOES NOT walk superclasses. Invokes const_missing
VALUE sorbet_getConstant(VALUE owner, ID name) {
    return rb_const_get_at(owner, name);
}

/*
VALUE sorbet_get_constant(std::string_view name) {
    if (name.size() < 2) {
        abort();
    }
    if (name[0] != ':' || name[1] != ':') {
        abort();
    }
    VALUE cnst = rb_cObject;
    std::size_t it = 2;
    std::size_t nexIt = name.find("::", it);
    while (nexIt != std::string_view::npos) {
        cnst = sorbet_getConstant(cnst,
sorbet_IDIntern(std::string(name.substr(it, nexIt)).c_str())); it = nexIt + 2;
        nexIt = name.find("::", it);
    }
    return cnst;
}
*/

VALUE sorbet_defineTopLevelModule(const char *name) {
    return rb_define_module(name);
}

VALUE sorbet_defineNestedModule(VALUE owner, const char *name) {
    return rb_define_module_under(owner, name);
}

VALUE sorbet_defineTopLevelClass(const char *name, VALUE super) {
    return rb_define_class(name, super);
}

VALUE sorbet_defineNestedClass(VALUE owner, const char *name, VALUE super) {
    return rb_define_class_under(owner, name, super);
}

// this DOES override existing methods
void sorbet_defineMethod(VALUE klass, const char *name, VALUE (*methodPtr)(ANYARGS), int argc) {
    rb_define_method(klass, name, methodPtr, argc);
}

// this DOES override existing methods
void sorbet_defineMethodSingleton(VALUE klass, const char *name, VALUE (*methodPtr)(ANYARGS), int argc) {
    rb_define_singleton_method(klass, name, methodPtr, argc);
}

// ****
// ****                       Calls
// ****

VALUE sorbet_callSuper(int argc, const VALUE *argv) {
    return rb_call_super(argc, argv);
}

VALUE sorbet_callBlock(VALUE array) {
    // TODO: one day we should use rb_yield_values, as it saves an allocation, but
    // for now, do the easy thing
    return rb_yield_splat(array);
}

VALUE sorbet_callFunc(VALUE recv, ID func, int argc, __attribute__((noescape)) const VALUE *const restrict argv)
    __attribute__((noinline)) {
    // TODO: use LLVM magic to make argv stack allocated
    return rb_funcallv(recv, func, argc, argv);
}

// defining a way to allocate storage for custom class:
//      VALUE allocate(VALUE klass);
//      rb_define_alloc_func(class, &allocate)
//

VALUE rb_arity_error_new(int argc, int min, int max) {
    VALUE err_mess = 0;
    if (min == max) {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d)", argc, min);
    } else if (max == UNLIMITED_ARGUMENTS) {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d+)", argc, min);
    } else {
        err_mess = rb_sprintf("wrong number of arguments (given %d, expected %d..%d)", argc, min, max);
    }
    return rb_exc_new3(rb_eArgError, err_mess);
}

void rb_error_arity(int argc, int min, int max) {
    rb_exc_raise(rb_arity_error_new(argc, min, max));
}

#endif
