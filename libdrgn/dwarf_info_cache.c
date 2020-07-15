// Copyright (c) Facebook, Inc. and its affiliates.
// SPDX-License-Identifier: GPL-3.0+

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <libelf.h>
#include <string.h>

#include "internal.h"
#include "dwarf_index.h"
#include "dwarf_info_cache.h"
#include "hash_table.h"
#include "object.h"
#include "object_index.h"
#include "program.h"
#include "type.h"
#include "vector.h"

DEFINE_HASH_TABLE_FUNCTIONS(dwarf_type_map, hash_pair_ptr_type,
			    hash_table_scalar_eq)

struct drgn_type_from_dwarf_thunk {
	struct drgn_type_thunk thunk;
	Dwarf_Die die;
	bool can_be_incomplete_array;
};

/**
 * Return whether a DWARF DIE is little-endian.
 *
 * @param[in] check_attr Whether to check the DW_AT_endianity attribute. If @c
 * false, only the ELF header is checked and this function cannot fail.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *dwarf_die_is_little_endian(Dwarf_Die *die,
						     bool check_attr, bool *ret)
{
	Dwarf_Attribute endianity_attr_mem, *endianity_attr;
	Dwarf_Word endianity;
	if (check_attr &&
	    (endianity_attr = dwarf_attr_integrate(die, DW_AT_endianity,
						   &endianity_attr_mem))) {
		if (dwarf_formudata(endianity_attr, &endianity)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_endianity");
		}
	} else {
		endianity = DW_END_default;
	}
	switch (endianity) {
	case DW_END_default: {
		Elf *elf = dwarf_getelf(dwarf_cu_getdwarf(die->cu));
		*ret = elf_getident(elf, NULL)[EI_DATA] == ELFDATA2LSB;
		return NULL;
	}
	case DW_END_little:
		*ret = true;
		return NULL;
	case DW_END_big:
		*ret = false;
		return NULL;
	default:
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "unknown DW_AT_endianity");
	}
}

/** Like dwarf_die_is_little_endian(), but returns a @ref drgn_byte_order. */
static struct drgn_error *dwarf_die_byte_order(Dwarf_Die *die,
					       bool check_attr,
					       enum drgn_byte_order *ret)
{
	bool little_endian;
	struct drgn_error *err = dwarf_die_is_little_endian(die, check_attr,
							    &little_endian);
	/*
	 * dwarf_die_is_little_endian() can't fail if check_attr is false, so
	 * the !check_attr test suppresses maybe-uninitialized warnings.
	 */
	if (!err || !check_attr)
		*ret = little_endian ? DRGN_LITTLE_ENDIAN : DRGN_BIG_ENDIAN;
	return err;
}

static int dwarf_type(Dwarf_Die *die, Dwarf_Die *ret)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	if (!(attr = dwarf_attr_integrate(die, DW_AT_type, &attr_mem)))
		return 1;

	return dwarf_formref_die(attr, ret) ? 0 : -1;
}

static int dwarf_flag(Dwarf_Die *die, unsigned int name, bool *ret)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	if (!(attr = dwarf_attr_integrate(die, name, &attr_mem))) {
		*ret = false;
		return 0;
	}
	return dwarf_formflag(attr, ret);
}

/**
 * Parse a type from a DWARF debugging information entry.
 *
 * This is the same as @ref drgn_type_from_dwarf() except that it can be used to
 * work around a bug in GCC < 9.0 that zero length array types are encoded the
 * same as incomplete array types. There are a few places where GCC allows
 * zero-length arrays but not incomplete arrays:
 *
 * - As the type of a member of a structure with only one member.
 * - As the type of a structure member other than the last member.
 * - As the type of a union member.
 * - As the element type of an array.
 *
 * In these cases, we know that what appears to be an incomplete array type must
 * actually have a length of zero. In other cases, a subrange DIE without
 * DW_AT_count or DW_AT_upper_bound is ambiguous; we return an incomplete array
 * type.
 *
 * @param[in] dicache Debugging information cache.
 * @param[in] die DIE to parse.
 * @param[in] can_be_incomplete_array Whether the type can be an incomplete
 * array type. If this is @c false and the type appears to be an incomplete
 * array type, its length is set to zero instead.
 * @param[out] is_incomplete_array_ret Whether the encoded type is an incomplete
 * array type or a typedef of an incomplete array type (regardless of @p
 * can_be_incomplete_array).
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static struct drgn_error *
drgn_type_from_dwarf_internal(struct drgn_dwarf_info_cache *dicache,
			      Dwarf_Die *die, bool can_be_incomplete_array,
			      bool *is_incomplete_array_ret,
			      struct drgn_qualified_type *ret);

/**
 * Parse a type from a DWARF debugging information entry.
 *
 * @param[in] dicache Debugging information cache.
 * @param[in] die DIE to parse.
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
static inline struct drgn_error *
drgn_type_from_dwarf(struct drgn_dwarf_info_cache *dicache, Dwarf_Die *die,
		     struct drgn_qualified_type *ret)
{
	return drgn_type_from_dwarf_internal(dicache, die, true, NULL, ret);
}

static struct drgn_error *
drgn_type_from_dwarf_thunk_evaluate_fn(struct drgn_type_thunk *thunk,
				       struct drgn_qualified_type *ret)
{
	struct drgn_type_from_dwarf_thunk *t =
		container_of(thunk, struct drgn_type_from_dwarf_thunk, thunk);
	return drgn_type_from_dwarf_internal(thunk->prog->_dicache, &t->die,
					     t->can_be_incomplete_array, NULL,
					     ret);
}

static void drgn_type_from_dwarf_thunk_free_fn(struct drgn_type_thunk *thunk)
{
	free(container_of(thunk, struct drgn_type_from_dwarf_thunk, thunk));
}

static struct drgn_error *
drgn_lazy_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			  Dwarf_Die *parent_die, bool can_be_incomplete_array,
			  const char *tag_name, struct drgn_lazy_type *ret)
{
	Dwarf_Attribute attr_mem, *attr;
	if (!(attr = dwarf_attr_integrate(parent_die, DW_AT_type, &attr_mem))) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s is missing DW_AT_type",
					 tag_name);
	}

	Dwarf_Die type_die;
	if (!dwarf_formref_die(attr, &type_die)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_type", tag_name);
	}

	struct drgn_type_from_dwarf_thunk *thunk = malloc(sizeof(*thunk));
	if (!thunk)
		return &drgn_enomem;

	thunk->thunk.prog = dicache->prog;
	thunk->thunk.evaluate_fn = drgn_type_from_dwarf_thunk_evaluate_fn;
	thunk->thunk.free_fn = drgn_type_from_dwarf_thunk_free_fn;
	thunk->die = type_die;
	thunk->can_be_incomplete_array = can_be_incomplete_array;
	drgn_lazy_type_init_thunk(ret, &thunk->thunk);
	return NULL;
}

/**
 * Parse a type from the @c DW_AT_type attribute of a DWARF debugging
 * information entry.
 *
 * @param[in] dicache Debugging information cache.
 * @param[in] parent_die Parent DIE.
 * @param[in] parent_lang Language of the parent DIE if it is already known, @c
 * NULL if it should be determined from @p parent_die.
 * @param[in] tag_name Spelling of the DWARF tag of @p parent_die. Used for
 * error messages.
 * @param[in] can_be_void Whether the @c DW_AT_type attribute may be missing,
 * which is interpreted as a void type. If this is false and the @c DW_AT_type
 * attribute is missing, an error is returned.
 * @param[in] can_be_incomplete_array See @ref drgn_type_from_dwarf_internal().
 * @param[in] is_incomplete_array_ret See @ref drgn_type_from_dwarf_internal().
 * @param[out] ret Returned type.
 * @return @c NULL on success, non-@c NULL on error.
 */
struct drgn_error *
drgn_type_from_dwarf_child(struct drgn_dwarf_info_cache *dicache,
			   Dwarf_Die *parent_die,
			   const struct drgn_language *parent_lang,
			   const char *tag_name,
			   bool can_be_void, bool can_be_incomplete_array,
			   bool *is_incomplete_array_ret,
			   struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;
	Dwarf_Die type_die;

	if (!(attr = dwarf_attr_integrate(parent_die, DW_AT_type, &attr_mem))) {
		if (can_be_void) {
			if (!parent_lang) {
				err = drgn_language_from_die(parent_die,
							     &parent_lang);
				if (err)
					return err;
			}
			ret->type = drgn_void_type(dicache->prog, parent_lang);
			ret->qualifiers = 0;
			return NULL;
		} else {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s is missing DW_AT_type",
						 tag_name);
		}
	}

	if (!dwarf_formref_die(attr, &type_die)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_type", tag_name);
	}

	return drgn_type_from_dwarf_internal(dicache, &type_die,
					     can_be_incomplete_array,
					     is_incomplete_array_ret, ret);
}

static struct drgn_error *
drgn_base_type_from_dwarf(struct drgn_dwarf_info_cache *dicache, Dwarf_Die *die,
			  const struct drgn_language *lang,
			  struct drgn_type **ret)
{
	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_name");
	}

	Dwarf_Attribute attr;
	Dwarf_Word encoding;
	if (!dwarf_attr_integrate(die, DW_AT_encoding, &attr) ||
	    dwarf_formudata(&attr, &encoding)) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_encoding");
	}
	int size = dwarf_bytesize(die);
	if (size == -1) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has missing or invalid DW_AT_byte_size");
	}

	switch (encoding) {
	case DW_ATE_boolean:
		return drgn_bool_type_create(dicache->prog, name, size, lang,
					     ret);
	case DW_ATE_float:
		return drgn_float_type_create(dicache->prog, name, size, lang,
					      ret);
	case DW_ATE_signed:
	case DW_ATE_signed_char:
		return drgn_int_type_create(dicache->prog, name, size, true,
					    lang, ret);
	case DW_ATE_unsigned:
	case DW_ATE_unsigned_char:
		return drgn_int_type_create(dicache->prog, name, size, false,
					    lang, ret);
	/*
	 * GCC also supports complex integer types, but DWARF 4 doesn't have an
	 * encoding for that. GCC as of 8.2 emits DW_ATE_lo_user, but that's
	 * ambiguous because it also emits that in other cases. For now, we
	 * don't support it.
	 */
	case DW_ATE_complex_float: {
		Dwarf_Die child;
		if (dwarf_type(die, &child)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_base_type has missing or invalid DW_AT_type");
		}
		struct drgn_qualified_type real_type;
		struct drgn_error *err = drgn_type_from_dwarf(dicache, &child,
							      &real_type);
		if (err)
			return err;
		if (drgn_type_kind(real_type.type) != DRGN_TYPE_FLOAT &&
		    drgn_type_kind(real_type.type) != DRGN_TYPE_INT) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_type of DW_ATE_complex_float is not a floating-point or integer type");
		}
		return drgn_complex_type_create(dicache->prog, name, size,
						real_type.type, lang, ret);
	}
	default:
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "DW_TAG_base_type has unknown DWARF encoding 0x%llx",
					 (unsigned long long)encoding);
	}
}

/*
 * DW_TAG_structure_type, DW_TAG_union_type, DW_TAG_class_type, and
 * DW_TAG_enumeration_type can be incomplete (i.e., have a DW_AT_declaration of
 * true). This tries to find the complete type. If it succeeds, it returns NULL.
 * If it can't find a complete type, it returns a DRGN_ERROR_STOP error.
 * Otherwise, it returns an error.
 */
static struct drgn_error *
drgn_dwarf_info_cache_find_complete(struct drgn_dwarf_info_cache *dicache,
				    uint64_t tag, const char *name,
				    struct drgn_type **ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_index_iterator it;
	Dwarf_Die die;
	struct drgn_qualified_type qualified_type;

	drgn_dwarf_index_iterator_init(&it, &dicache->dindex, name,
				       strlen(name), &tag, 1);
	/*
	 * Find a matching DIE. Note that drgn_dwarf_index does not contain DIEs
	 * with DW_AT_declaration, so this will always be a complete type.
	 */
	err = drgn_dwarf_index_iterator_next(&it, &die, NULL);
	if (err)
		return err;
	/*
	 * Look for another matching DIE. If there is one, then we can't be sure
	 * which type this is, so leave it incomplete rather than guessing.
	 */
	err = drgn_dwarf_index_iterator_next(&it, &die, NULL);
	if (!err)
		return &drgn_stop;
	else if (err->code != DRGN_ERROR_STOP)
		return err;

	err = drgn_type_from_dwarf(dicache, &die, &qualified_type);
	if (err)
		return err;
	*ret = qualified_type.type;
	return NULL;
}

static struct drgn_error *
parse_member_offset(Dwarf_Die *die, struct drgn_lazy_type *member_type,
		    uint64_t bit_field_size, bool little_endian, uint64_t *ret)
{
	struct drgn_error *err;
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;

	/*
	 * The simplest case is when we have DW_AT_data_bit_offset, which is
	 * already the offset in bits from the beginning of the containing
	 * object to the beginning of the member (which may be a bit field).
	 */
	attr = dwarf_attr_integrate(die, DW_AT_data_bit_offset, &attr_mem);
	if (attr) {
		Dwarf_Word bit_offset;

		if (dwarf_formudata(attr, &bit_offset)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_data_bit_offset");
		}
		*ret = bit_offset;
		return NULL;
	}

	/*
	 * Otherwise, we might have DW_AT_data_member_location, which is the
	 * offset in bytes from the beginning of the containing object.
	 */
	attr = dwarf_attr_integrate(die, DW_AT_data_member_location, &attr_mem);
	if (attr) {
		Dwarf_Word byte_offset;

		if (dwarf_formudata(attr, &byte_offset)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_data_member_location");
		}
		*ret = 8 * byte_offset;
	} else {
		*ret = 0;
	}

	/*
	 * In addition to DW_AT_data_member_location, a bit field might have
	 * DW_AT_bit_offset, which is the offset in bits of the most significant
	 * bit of the bit field from the most significant bit of the containing
	 * object.
	 */
	attr = dwarf_attr_integrate(die, DW_AT_bit_offset, &attr_mem);
	if (attr) {
		Dwarf_Word bit_offset;

		if (dwarf_formudata(attr, &bit_offset)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_bit_offset");
		}

		/*
		 * If the architecture is little-endian, then we must compute
		 * the location of the most significant bit from the size of the
		 * member, then subtract the bit offset and bit size to get the
		 * location of the beginning of the bit field.
		 *
		 * If the architecture is big-endian, then the most significant
		 * bit of the bit field is the beginning.
		 */
		if (little_endian) {
			uint64_t byte_size;

			attr = dwarf_attr_integrate(die, DW_AT_byte_size,
						    &attr_mem);
			/*
			 * If the member has an explicit byte size, we can use
			 * that. Otherwise, we have to get it from the member
			 * type.
			 */
			if (attr) {
				Dwarf_Word word;

				if (dwarf_formudata(attr, &word)) {
					return drgn_error_create(DRGN_ERROR_OTHER,
								 "DW_TAG_member has invalid DW_AT_byte_size");
				}
				byte_size = word;
			} else {
				struct drgn_qualified_type containing_type;

				err = drgn_lazy_type_evaluate(member_type,
							      &containing_type);
				if (err)
					return err;
				if (!drgn_type_has_size(containing_type.type)) {
					return drgn_error_create(DRGN_ERROR_OTHER,
								 "DW_TAG_member bit field type does not have size");
				}
				byte_size = drgn_type_size(containing_type.type);
			}
			*ret += 8 * byte_size - bit_offset - bit_field_size;
		} else {
			*ret += bit_offset;
		}
	}

	return NULL;
}

static struct drgn_error *
parse_member(struct drgn_dwarf_info_cache *dicache, Dwarf_Die *die,
	     bool little_endian, bool can_be_incomplete_array,
	     struct drgn_compound_type_builder *builder)
{
	Dwarf_Attribute attr_mem, *attr;
	const char *name;
	if ((attr = dwarf_attr_integrate(die, DW_AT_name, &attr_mem))) {
		name = dwarf_formstring(attr);
		if (!name) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_name");
		}
	} else {
		name = NULL;
	}

	uint64_t bit_field_size;
	if ((attr = dwarf_attr_integrate(die, DW_AT_bit_size, &attr_mem))) {
		Dwarf_Word bit_size;
		if (dwarf_formudata(attr, &bit_size)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_member has invalid DW_AT_bit_size");
		}
		bit_field_size = bit_size;
	} else {
		bit_field_size = 0;
	}

	struct drgn_lazy_type member_type;
	struct drgn_error *err = drgn_lazy_type_from_dwarf(dicache, die,
							   can_be_incomplete_array,
							   "DW_TAG_member",
							   &member_type);
	if (err)
		return err;

	uint64_t bit_offset;
	err = parse_member_offset(die, &member_type, bit_field_size,
				  little_endian, &bit_offset);
	if (err)
		goto err;

	err = drgn_compound_type_builder_add_member(builder, member_type, name,
						    bit_offset, bit_field_size);
	if (err)
		goto err;
	return NULL;

err:
	drgn_lazy_type_deinit(&member_type);
	return err;
}

static struct drgn_error *
drgn_compound_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			      Dwarf_Die *die, const struct drgn_language *lang,
			      enum drgn_type_kind kind, struct drgn_type **ret)
{
	struct drgn_error *err;

	const char *dw_tag_str;
	uint64_t dw_tag;
	switch (kind) {
	case DRGN_TYPE_STRUCT:
		dw_tag_str = "DW_TAG_structure_type";
		dw_tag = DW_TAG_structure_type;
		break;
	case DRGN_TYPE_UNION:
		dw_tag_str = "DW_TAG_union_type";
		dw_tag = DW_TAG_union_type;
		break;
	case DRGN_TYPE_CLASS:
		dw_tag_str = "DW_TAG_class_type";
		dw_tag = DW_TAG_class_type;
		break;
	default:
		UNREACHABLE();
	}

	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr = dwarf_attr_integrate(die, DW_AT_name,
						     &attr_mem);
	const char *tag;
	if (attr) {
		tag = dwarf_formstring(attr);
		if (!tag) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "%s has invalid DW_AT_name",
						 dw_tag_str);
		}
	} else {
		tag = NULL;
	}

	bool declaration;
	if (dwarf_flag(die, DW_AT_declaration, &declaration)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has invalid DW_AT_declaration",
					 dw_tag_str);
	}
	if (declaration && tag) {
		err = drgn_dwarf_info_cache_find_complete(dicache, dw_tag, tag,
							  ret);
		if (!err || err->code != DRGN_ERROR_STOP)
			return err;
	}

	if (declaration) {
		return drgn_incomplete_compound_type_create(dicache->prog, kind,
							    tag, lang, ret);
	}

	int size = dwarf_bytesize(die);
	if (size == -1) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "%s has missing or invalid DW_AT_byte_size",
					 dw_tag_str);
	}

	struct drgn_compound_type_builder builder;
	drgn_compound_type_builder_init(&builder, dicache->prog, kind);
	bool little_endian;
	dwarf_die_is_little_endian(die, false, &little_endian);
	Dwarf_Die member = {}, child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		if (dwarf_tag(&child) == DW_TAG_member) {
			if (member.addr) {
				err = parse_member(dicache, &member,
						   little_endian, false,
						   &builder);
				if (err)
					goto err;
			}
			member = child;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}
	/*
	 * Flexible array members are only allowed as the last member of a
	 * structure with at least one other member.
	 */
	if (member.addr) {
		err = parse_member(dicache, &member, little_endian,
				   kind != DRGN_TYPE_UNION &&
				   builder.members.size > 0,
				   &builder);
		if (err)
			goto err;
	}

	err = drgn_compound_type_create(&builder, tag, size, lang, ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_compound_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
parse_enumerator(Dwarf_Die *die, struct drgn_enum_type_builder *builder,
		 bool *is_signed)
{
	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumerator has missing or invalid DW_AT_name");
	}

	Dwarf_Attribute attr_mem, *attr;
	if (!(attr = dwarf_attr_integrate(die, DW_AT_const_value, &attr_mem))) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumerator is missing DW_AT_const_value");
	}
	struct drgn_error *err;
	if (attr->form == DW_FORM_sdata ||
	    attr->form == DW_FORM_implicit_const) {
		Dwarf_Sword svalue;
		if (dwarf_formsdata(attr, &svalue))
			goto invalid;
		err = drgn_enum_type_builder_add_signed(builder, name,
							svalue);
		/*
		 * GCC before 7.1 didn't include DW_AT_encoding for
		 * DW_TAG_enumeration_type DIEs, so we have to guess the sign
		 * for enum_compatible_type_fallback().
		 */
		if (!err && svalue < 0)
			*is_signed = true;
	} else {
		Dwarf_Word uvalue;
		if (dwarf_formudata(attr, &uvalue))
			goto invalid;
		err = drgn_enum_type_builder_add_unsigned(builder, name,
							  uvalue);
	}
	return err;

invalid:
	return drgn_error_create(DRGN_ERROR_OTHER,
				 "DW_TAG_enumerator has invalid DW_AT_const_value");
}

/*
 * GCC before 5.1 did not include DW_AT_type for DW_TAG_enumeration_type DIEs,
 * so we have to fabricate the compatible type.
 */
static struct drgn_error *
enum_compatible_type_fallback(struct drgn_dwarf_info_cache *dicache,
			      Dwarf_Die *die, bool is_signed,
			      const struct drgn_language *lang,
			      struct drgn_type **ret)
{
	int size = dwarf_bytesize(die);
	if (size == -1) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumeration_type has missing or invalid DW_AT_byte_size");
	}
	return drgn_int_type_create(dicache->prog, "<unknown>", size, is_signed,
				    lang, ret);
}

static struct drgn_error *
drgn_enum_type_from_dwarf(struct drgn_dwarf_info_cache *dicache, Dwarf_Die *die,
			  const struct drgn_language *lang,
			  struct drgn_type **ret)
{
	struct drgn_error *err;

	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr = dwarf_attr_integrate(die, DW_AT_name,
						     &attr_mem);
	const char *tag;
	if (attr) {
		tag = dwarf_formstring(attr);
		if (!tag)
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_enumeration_type has invalid DW_AT_name");
	} else {
		tag = NULL;
	}

	bool declaration;
	if (dwarf_flag(die, DW_AT_declaration, &declaration)) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_enumeration_type has invalid DW_AT_declaration");
	}
	if (declaration && tag) {
		err = drgn_dwarf_info_cache_find_complete(dicache,
							  DW_TAG_enumeration_type,
							  tag, ret);
		if (!err || err->code != DRGN_ERROR_STOP)
			return err;
	}

	if (declaration) {
		return drgn_incomplete_enum_type_create(dicache->prog, tag,
							lang, ret);
	}

	struct drgn_enum_type_builder builder;
	drgn_enum_type_builder_init(&builder, dicache->prog);
	bool is_signed = false;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		if (dwarf_tag(&child) == DW_TAG_enumerator) {
			err = parse_enumerator(&child, &builder, &is_signed);
			if (err)
				goto err;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}

	struct drgn_type *compatible_type;
	r = dwarf_type(die, &child);
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"DW_TAG_enumeration_type has invalid DW_AT_type");
		goto err;
	} else if (r) {
		err = enum_compatible_type_fallback(dicache, die, is_signed,
						    lang, &compatible_type);
		if (err)
			goto err;
	} else {
		struct drgn_qualified_type qualified_compatible_type;
		err = drgn_type_from_dwarf(dicache, &child,
					   &qualified_compatible_type);
		if (err)
			goto err;
		compatible_type = qualified_compatible_type.type;
		if (drgn_type_kind(compatible_type) != DRGN_TYPE_INT) {
			err = drgn_error_create(DRGN_ERROR_OTHER,
						"DW_AT_type of DW_TAG_enumeration_type is not an integer type");
			goto err;
		}
	}

	err = drgn_enum_type_create(&builder, tag, compatible_type, lang, ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_enum_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
drgn_typedef_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			     Dwarf_Die *die,
			     const struct drgn_language *lang,
			     bool can_be_incomplete_array,
			     bool *is_incomplete_array_ret,
			     struct drgn_type **ret)
{
	const char *name = dwarf_diename(die);
	if (!name) {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "DW_TAG_typedef has missing or invalid DW_AT_name");
	}

	struct drgn_qualified_type aliased_type;
	struct drgn_error *err = drgn_type_from_dwarf_child(dicache, die,
							    drgn_language_or_default(lang),
							    "DW_TAG_typedef",
							    true,
							    can_be_incomplete_array,
							    is_incomplete_array_ret,
							    &aliased_type);
	if (err)
		return err;

	return drgn_typedef_type_create(dicache->prog, name, aliased_type, lang,
					ret);
}

static struct drgn_error *
drgn_pointer_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			     Dwarf_Die *die, const struct drgn_language *lang,
			     struct drgn_type **ret)
{
	struct drgn_qualified_type referenced_type;
	struct drgn_error *err = drgn_type_from_dwarf_child(dicache, die,
							    drgn_language_or_default(lang),
							    "DW_TAG_pointer_type",
							    true, true, NULL,
							    &referenced_type);
	if (err)
		return err;

	Dwarf_Attribute attr_mem, *attr;
	uint64_t size;
	if ((attr = dwarf_attr_integrate(die, DW_AT_byte_size, &attr_mem))) {
		Dwarf_Word word;
		if (dwarf_formudata(attr, &word)) {
			return drgn_error_format(DRGN_ERROR_OTHER,
						 "DW_TAG_pointer_type has invalid DW_AT_byte_size");
		}
		size = word;
	} else {
		uint8_t word_size;
		err = drgn_program_word_size(dicache->prog, &word_size);
		if (err)
			return err;
		size = word_size;
	}

	return drgn_pointer_type_create(dicache->prog, referenced_type, size,
					lang, ret);
}

struct array_dimension {
	uint64_t length;
	bool is_complete;
};

DEFINE_VECTOR(array_dimension_vector, struct array_dimension)

static struct drgn_error *subrange_length(Dwarf_Die *die,
					  struct array_dimension *dimension)
{
	Dwarf_Attribute attr_mem;
	Dwarf_Attribute *attr;
	Dwarf_Word word;

	if (!(attr = dwarf_attr_integrate(die, DW_AT_upper_bound, &attr_mem)) &&
	    !(attr = dwarf_attr_integrate(die, DW_AT_count, &attr_mem))) {
		dimension->is_complete = false;
		return NULL;
	}

	if (dwarf_formudata(attr, &word)) {
		return drgn_error_format(DRGN_ERROR_OTHER,
					 "DW_TAG_subrange_type has invalid %s",
					 attr->code == DW_AT_upper_bound ?
					 "DW_AT_upper_bound" :
					 "DW_AT_count");
	}

	dimension->is_complete = true;
	/*
	 * GCC emits a DW_FORM_sdata DW_AT_upper_bound of -1 for empty array
	 * variables without an explicit size (e.g., `int arr[] = {};`).
	 */
	if (attr->code == DW_AT_upper_bound && attr->form == DW_FORM_sdata &&
	    word == (Dwarf_Word)-1) {
		dimension->length = 0;
	} else if (attr->code == DW_AT_upper_bound) {
		if (word >= UINT64_MAX) {
			return drgn_error_create(DRGN_ERROR_OVERFLOW,
						 "DW_AT_upper_bound is too large");
		}
		dimension->length = (uint64_t)word + 1;
	} else {
		if (word > UINT64_MAX) {
			return drgn_error_create(DRGN_ERROR_OVERFLOW,
						 "DW_AT_count is too large");
		}
		dimension->length = word;
	}
	return NULL;
}

static struct drgn_error *
drgn_array_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			   Dwarf_Die *die, const struct drgn_language *lang,
			   bool can_be_incomplete_array,
			   bool *is_incomplete_array_ret,
			   struct drgn_type **ret)
{
	struct drgn_error *err;
	struct array_dimension_vector dimensions = VECTOR_INIT;
	struct array_dimension *dimension;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		if (dwarf_tag(&child) == DW_TAG_subrange_type) {
			dimension = array_dimension_vector_append_entry(&dimensions);
			if (!dimension)
				goto out;
			err = subrange_length(&child, dimension);
			if (err)
				goto out;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto out;
	}
	if (!dimensions.size) {
		dimension = array_dimension_vector_append_entry(&dimensions);
		if (!dimension)
			goto out;
		dimension->is_complete = false;
	}

	struct drgn_qualified_type element_type;
	err = drgn_type_from_dwarf_child(dicache, die,
					 drgn_language_or_default(lang),
					 "DW_TAG_array_type", false, false,
					 NULL, &element_type);
	if (err)
		goto out;

	*is_incomplete_array_ret = !dimensions.data[0].is_complete;
	struct drgn_type *type;
	do {
		dimension = array_dimension_vector_pop(&dimensions);
		if (dimension->is_complete) {
			err = drgn_array_type_create(dicache->prog,
						     element_type,
						     dimension->length, lang,
						     &type);
		} else if (dimensions.size || !can_be_incomplete_array) {
			err = drgn_array_type_create(dicache->prog,
						     element_type, 0, lang,
						     &type);
		} else {
			err = drgn_incomplete_array_type_create(dicache->prog,
								element_type,
								lang, &type);
		}
		if (err)
			goto out;

		element_type.type = type;
		element_type.qualifiers = 0;
	} while (dimensions.size);

	*ret = type;
	err = NULL;
out:
	array_dimension_vector_deinit(&dimensions);
	return err;
}

static struct drgn_error *
parse_formal_parameter(struct drgn_dwarf_info_cache *dicache, Dwarf_Die *die,
		       struct drgn_function_type_builder *builder)
{
	Dwarf_Attribute attr_mem, *attr;
	const char *name;
	if ((attr = dwarf_attr_integrate(die, DW_AT_name, &attr_mem))) {
		name = dwarf_formstring(attr);
		if (!name) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_TAG_formal_parameter has invalid DW_AT_name");
		}
	} else {
		name = NULL;
	}

	struct drgn_lazy_type parameter_type;
	struct drgn_error *err = drgn_lazy_type_from_dwarf(dicache, die, true,
							   "DW_TAG_formal_parameter",
							   &parameter_type);
	if (err)
		return err;

	err = drgn_function_type_builder_add_parameter(builder, parameter_type,
						       name);
	if (err)
		drgn_lazy_type_deinit(&parameter_type);
	return err;
}

static struct drgn_error *
drgn_function_type_from_dwarf(struct drgn_dwarf_info_cache *dicache,
			      Dwarf_Die *die, const struct drgn_language *lang,
			      struct drgn_type **ret)
{
	struct drgn_error *err;

	const char *tag_name =
		dwarf_tag(die) == DW_TAG_subroutine_type ?
		"DW_TAG_subroutine_type" : "DW_TAG_subprogram";
	struct drgn_function_type_builder builder;
	drgn_function_type_builder_init(&builder, dicache->prog);
	bool is_variadic = false;
	Dwarf_Die child;
	int r = dwarf_child(die, &child);
	while (r == 0) {
		switch (dwarf_tag(&child)) {
		case DW_TAG_formal_parameter:
			if (is_variadic) {
				err = drgn_error_format(DRGN_ERROR_OTHER,
							"%s has DW_TAG_formal_parameter child after DW_TAG_unspecified_parameters child",
							tag_name);
				goto err;
			}
			err = parse_formal_parameter(dicache, &child, &builder);
			if (err)
				goto err;
			break;
		case DW_TAG_unspecified_parameters:
			if (is_variadic) {
				err = drgn_error_format(DRGN_ERROR_OTHER,
							"%s has multiple DW_TAG_unspecified_parameters children",
							tag_name);
				goto err;
			}
			is_variadic = true;
			break;
		default:
			break;
		}
		r = dwarf_siblingof(&child, &child);
	}
	if (r == -1) {
		err = drgn_error_create(DRGN_ERROR_OTHER,
					"libdw could not parse DIE children");
		goto err;
	}

	struct drgn_qualified_type return_type;
	err = drgn_type_from_dwarf_child(dicache, die,
					 drgn_language_or_default(lang),
					 tag_name, true, true, NULL,
					 &return_type);
	if (err)
		goto err;

	err = drgn_function_type_create(&builder, return_type, is_variadic,
					lang, ret);
	if (err)
		goto err;
	return NULL;

err:
	drgn_function_type_builder_deinit(&builder);
	return err;
}

static struct drgn_error *
drgn_type_from_dwarf_internal(struct drgn_dwarf_info_cache *dicache,
			      Dwarf_Die *die, bool can_be_incomplete_array,
			      bool *is_incomplete_array_ret,
			      struct drgn_qualified_type *ret)
{
	if (dicache->depth >= 1000) {
		return drgn_error_create(DRGN_ERROR_RECURSION,
					 "maximum DWARF type parsing depth exceeded");
	}

	struct dwarf_type_map_entry entry = {
		.key = die->addr,
	};
	struct hash_pair hp = dwarf_type_map_hash(&entry.key);
	struct dwarf_type_map_iterator it =
		dwarf_type_map_search_hashed(&dicache->map, &entry.key, hp);
	if (it.entry) {
		if (!can_be_incomplete_array &&
		    it.entry->value.is_incomplete_array) {
			it = dwarf_type_map_search_hashed(&dicache->cant_be_incomplete_array_map,
							  &entry.key, hp);
		}
		if (it.entry) {
			ret->type = it.entry->value.type;
			ret->qualifiers = it.entry->value.qualifiers;
			return NULL;
		}
	}

	const struct drgn_language *lang;
	struct drgn_error *err = drgn_language_from_die(die, &lang);
	if (err)
		return err;

	ret->qualifiers = 0;
	dicache->depth++;
	entry.value.is_incomplete_array = false;
	switch (dwarf_tag(die)) {
	case DW_TAG_const_type:
		err = drgn_type_from_dwarf_child(dicache, die,
						 drgn_language_or_default(lang),
						 "DW_TAG_const_type", true,
						 true, NULL, ret);
		ret->qualifiers |= DRGN_QUALIFIER_CONST;
		break;
	case DW_TAG_restrict_type:
		err = drgn_type_from_dwarf_child(dicache, die,
						 drgn_language_or_default(lang),
						 "DW_TAG_restrict_type", true,
						 true, NULL, ret);
		ret->qualifiers |= DRGN_QUALIFIER_RESTRICT;
		break;
	case DW_TAG_volatile_type:
		err = drgn_type_from_dwarf_child(dicache, die,
						 drgn_language_or_default(lang),
						 "DW_TAG_volatile_type", true,
						 true, NULL, ret);
		ret->qualifiers |= DRGN_QUALIFIER_VOLATILE;
		break;
	case DW_TAG_atomic_type:
		err = drgn_type_from_dwarf_child(dicache, die,
						 drgn_language_or_default(lang),
						 "DW_TAG_atomic_type", true,
						 true, NULL, ret);
		ret->qualifiers |= DRGN_QUALIFIER_ATOMIC;
		break;
	case DW_TAG_base_type:
		err = drgn_base_type_from_dwarf(dicache, die, lang, &ret->type);
		break;
	case DW_TAG_structure_type:
		err = drgn_compound_type_from_dwarf(dicache, die, lang,
						    DRGN_TYPE_STRUCT,
						    &ret->type);
		break;
	case DW_TAG_union_type:
		err = drgn_compound_type_from_dwarf(dicache, die, lang,
						    DRGN_TYPE_UNION,
						    &ret->type);
		break;
	case DW_TAG_class_type:
		err = drgn_compound_type_from_dwarf(dicache, die, lang,
						    DRGN_TYPE_CLASS,
						    &ret->type);
		break;
	case DW_TAG_enumeration_type:
		err = drgn_enum_type_from_dwarf(dicache, die, lang, &ret->type);
		break;
	case DW_TAG_typedef:
		err = drgn_typedef_type_from_dwarf(dicache, die, lang,
						   can_be_incomplete_array,
						   &entry.value.is_incomplete_array,
						   &ret->type);
		break;
	case DW_TAG_pointer_type:
		err = drgn_pointer_type_from_dwarf(dicache, die, lang,
						   &ret->type);
		break;
	case DW_TAG_array_type:
		err = drgn_array_type_from_dwarf(dicache, die, lang,
						 can_be_incomplete_array,
						 &entry.value.is_incomplete_array,
						 &ret->type);
		break;
	case DW_TAG_subroutine_type:
	case DW_TAG_subprogram:
		err = drgn_function_type_from_dwarf(dicache, die, lang,
						    &ret->type);
		break;
	default:
		err = drgn_error_format(DRGN_ERROR_OTHER,
					"unknown DWARF type tag 0x%x",
					dwarf_tag(die));
		break;
	}
	dicache->depth--;
	if (err)
		return err;

	entry.value.type = ret->type;
	entry.value.qualifiers = ret->qualifiers;
	struct dwarf_type_map *map;
	if (!can_be_incomplete_array && entry.value.is_incomplete_array)
		map = &dicache->cant_be_incomplete_array_map;
	else
		map = &dicache->map;
	/* TODO: reserve so this won't fail? Ignore it? At least comment. */
	if (dwarf_type_map_insert_searched(map, &entry, hp, NULL) == -1)
		return &drgn_enomem;
	if (is_incomplete_array_ret)
		*is_incomplete_array_ret = entry.value.is_incomplete_array;
	return NULL;
}

struct drgn_error *drgn_dwarf_type_find(enum drgn_type_kind kind,
					const char *name, size_t name_len,
					const char *filename, void *arg,
					struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_info_cache *dicache = arg;
	struct drgn_dwarf_index_iterator it;
	Dwarf_Die die;
	uint64_t tag;

	switch (kind) {
	case DRGN_TYPE_INT:
	case DRGN_TYPE_BOOL:
	case DRGN_TYPE_FLOAT:
		tag = DW_TAG_base_type;
		break;
	case DRGN_TYPE_STRUCT:
		tag = DW_TAG_structure_type;
		break;
	case DRGN_TYPE_UNION:
		tag = DW_TAG_union_type;
		break;
	case DRGN_TYPE_CLASS:
		tag = DW_TAG_class_type;
		break;
	case DRGN_TYPE_ENUM:
		tag = DW_TAG_enumeration_type;
		break;
	case DRGN_TYPE_TYPEDEF:
		tag = DW_TAG_typedef;
		break;
	default:
		UNREACHABLE();
	}

	drgn_dwarf_index_iterator_init(&it, &dicache->dindex, name, name_len,
				       &tag, 1);
	while (!(err = drgn_dwarf_index_iterator_next(&it, &die, NULL))) {
		if (die_matches_filename(&die, filename)) {
			err = drgn_type_from_dwarf(dicache, &die, ret);
			if (err)
				return err;
			/*
			 * For DW_TAG_base_type, we need to check that the type
			 * we found was the right kind.
			 */
			if (drgn_type_kind(ret->type) == kind)
				return NULL;
		}
	}
	if (err && err->code != DRGN_ERROR_STOP)
		return err;
	return &drgn_not_found;
}

static struct drgn_error *
drgn_object_from_dwarf_enumerator(struct drgn_dwarf_info_cache *dicache,
				  Dwarf_Die *die, const char *name,
				  struct drgn_object *ret)
{
	struct drgn_error *err;
	struct drgn_qualified_type qualified_type;
	const struct drgn_type_enumerator *enumerators;
	size_t num_enumerators, i;

	err = drgn_type_from_dwarf(dicache, die, &qualified_type);
	if (err)
		return err;
	enumerators = drgn_type_enumerators(qualified_type.type);
	num_enumerators = drgn_type_num_enumerators(qualified_type.type);
	for (i = 0; i < num_enumerators; i++) {
		if (strcmp(enumerators[i].name, name) != 0)
			continue;

		if (drgn_enum_type_is_signed(qualified_type.type)) {
			return drgn_object_set_signed(ret, qualified_type,
						      enumerators[i].svalue, 0);
		} else {
			return drgn_object_set_unsigned(ret, qualified_type,
							enumerators[i].uvalue,
							0);
		}
	}
	UNREACHABLE();
}

static struct drgn_error *
drgn_object_from_dwarf_subprogram(struct drgn_dwarf_info_cache *dicache,
				  Dwarf_Die *die, uint64_t bias,
				  const char *name, struct drgn_object *ret)
{
	struct drgn_qualified_type qualified_type;
	struct drgn_error *err = drgn_type_from_dwarf(dicache, die,
						      &qualified_type);
	if (err)
		return err;
	Dwarf_Addr low_pc;
	if (dwarf_lowpc(die, &low_pc) == -1) {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "could not find address of '%s'",
					 name);
	}
	enum drgn_byte_order byte_order;
	dwarf_die_byte_order(die, false, &byte_order);
	return drgn_object_set_reference(ret, qualified_type, low_pc + bias, 0,
					 0, byte_order);
}

static struct drgn_error *
drgn_object_from_dwarf_constant(struct drgn_dwarf_info_cache *dicache,
				Dwarf_Die *die,
				struct drgn_qualified_type qualified_type,
				Dwarf_Attribute *attr, struct drgn_object *ret)
{
	struct drgn_object_type type;
	enum drgn_object_kind kind;
	uint64_t bit_size;
	struct drgn_error *err = drgn_object_set_common(qualified_type, 0,
							&type, &kind,
							&bit_size);
	if (err)
		return err;
	Dwarf_Block block;
	if (dwarf_formblock(attr, &block) == 0) {
		bool little_endian;
		err = dwarf_die_is_little_endian(die, true, &little_endian);
		if (err)
			return err;
		if (block.length < drgn_value_size(bit_size, 0)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_const_value block is too small");
		}
		return drgn_object_set_buffer_internal(ret, &type, kind,
						       bit_size, block.data, 0,
						       little_endian);
	} else if (kind == DRGN_OBJECT_SIGNED) {
		Dwarf_Sword svalue;
		if (dwarf_formsdata(attr, &svalue)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_const_value");
		}
		return drgn_object_set_signed_internal(ret, &type, bit_size,
						       svalue);
	} else if (kind == DRGN_OBJECT_UNSIGNED) {
		Dwarf_Word uvalue;
		if (dwarf_formudata(attr, &uvalue)) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "invalid DW_AT_const_value");
		}
		return drgn_object_set_unsigned_internal(ret, &type, bit_size,
							 uvalue);
	} else {
		return drgn_error_create(DRGN_ERROR_OTHER,
					 "unknown DW_AT_const_value form");
	}
}

static struct drgn_error *
drgn_object_from_dwarf_variable(struct drgn_dwarf_info_cache *dicache,
				Dwarf_Die *die, uint64_t bias, const char *name,
				struct drgn_object *ret)
{
	struct drgn_qualified_type qualified_type;
	struct drgn_error *err = drgn_type_from_dwarf_child(dicache, die, NULL,
							    "DW_TAG_variable",
							    true, true, NULL,
							    &qualified_type);
	if (err)
		return err;
	Dwarf_Attribute attr_mem, *attr;
	if ((attr = dwarf_attr_integrate(die, DW_AT_location, &attr_mem))) {
		Dwarf_Op *loc;
		size_t nloc;
		if (dwarf_getlocation(attr, &loc, &nloc))
			return drgn_error_libdw();
		if (nloc != 1 || loc[0].atom != DW_OP_addr) {
			return drgn_error_create(DRGN_ERROR_OTHER,
						 "DW_AT_location has unimplemented operation");
		}
		enum drgn_byte_order byte_order;
		err = dwarf_die_byte_order(die, true, &byte_order);
		if (err)
			return err;
		return drgn_object_set_reference(ret, qualified_type,
						 loc[0].number + bias, 0, 0,
						 byte_order);
	} else if ((attr = dwarf_attr_integrate(die, DW_AT_const_value,
						&attr_mem))) {
		return drgn_object_from_dwarf_constant(dicache, die,
						       qualified_type, attr,
						       ret);
	} else {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "could not find address or value of '%s'",
					 name);
	}
}

struct drgn_error *
drgn_dwarf_object_find(const char *name, size_t name_len, const char *filename,
		       enum drgn_find_object_flags flags, void *arg,
		       struct drgn_object *ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_info_cache *dicache = arg;
	uint64_t tags[3];
	size_t num_tags;
	struct drgn_dwarf_index_iterator it;
	Dwarf_Die die;
	uint64_t bias;

	num_tags = 0;
	if (flags & DRGN_FIND_OBJECT_CONSTANT)
		tags[num_tags++] = DW_TAG_enumerator;
	if (flags & DRGN_FIND_OBJECT_FUNCTION)
		tags[num_tags++] = DW_TAG_subprogram;
	if (flags & DRGN_FIND_OBJECT_VARIABLE)
		tags[num_tags++] = DW_TAG_variable;

	drgn_dwarf_index_iterator_init(&it, &dicache->dindex, name,
				       strlen(name), tags, num_tags);
	while (!(err = drgn_dwarf_index_iterator_next(&it, &die, &bias))) {
		if (!die_matches_filename(&die, filename))
			continue;
		switch (dwarf_tag(&die)) {
		case DW_TAG_enumeration_type:
			return drgn_object_from_dwarf_enumerator(dicache, &die,
								 name, ret);
		case DW_TAG_subprogram:
			return drgn_object_from_dwarf_subprogram(dicache, &die,
								 bias, name,
								 ret);
		case DW_TAG_variable:
			return drgn_object_from_dwarf_variable(dicache, &die,
							       bias, name, ret);
		default:
			UNREACHABLE();
		}
	}
	if (err && err->code != DRGN_ERROR_STOP)
		return err;
	return &drgn_not_found;
}

struct drgn_error *
drgn_dwarf_info_cache_create(struct drgn_program *prog,
			     const Dwfl_Callbacks *dwfl_callbacks,
			     struct drgn_dwarf_info_cache **ret)
{
	struct drgn_error *err;
	struct drgn_dwarf_info_cache *dicache;

	dicache = malloc(sizeof(*dicache));
	if (!dicache)
		return &drgn_enomem;
	err = drgn_dwarf_index_init(&dicache->dindex, dwfl_callbacks);
	if (err) {
		free(dicache);
		return err;
	}
	dwarf_type_map_init(&dicache->map);
	dwarf_type_map_init(&dicache->cant_be_incomplete_array_map);
	dicache->depth = 0;
	dicache->prog = prog;
	*ret = dicache;
	return NULL;
}

void drgn_dwarf_info_cache_destroy(struct drgn_dwarf_info_cache *dicache)
{
	if (!dicache)
		return;
	dwarf_type_map_deinit(&dicache->cant_be_incomplete_array_map);
	dwarf_type_map_deinit(&dicache->map);
	drgn_dwarf_index_deinit(&dicache->dindex);
	free(dicache);
}
