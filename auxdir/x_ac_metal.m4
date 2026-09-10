dnl Detect the optional native macOS GPU discovery backend.
AC_DEFUN([X_AC_METAL], [
	AC_REQUIRE([AC_PROG_OBJC])
	AC_ARG_ENABLE([metal],
		AS_HELP_STRING([--enable-metal@<:@=auto/yes/no@:>@],
			[build native macOS Metal GPU discovery @<:@auto@:>@]),
		[], [enable_metal=auto])
	AS_CASE([$enable_metal], [auto|yes|no], [],
		[AC_MSG_ERROR([--enable-metal expects auto, yes, or no])])

	METAL_OBJCFLAGS=""
	METAL_LIBS=""
	have_metal=no
	AS_IF([test "x$darwin_build" = xyes &&
	       test "x$enable_metal" != xno], [
		slurm_save_OBJCFLAGS=$OBJCFLAGS
		slurm_save_LIBS=$LIBS
		METAL_OBJCFLAGS="-fobjc-arc -Werror=unguarded-availability-new"
		METAL_LIBS="-framework Foundation -framework Metal"
		OBJCFLAGS="$OBJCFLAGS $METAL_OBJCFLAGS"
		LIBS="$METAL_LIBS $LIBS"
		AC_LANG_PUSH([Objective C])
		AC_MSG_CHECKING([for the required Metal device APIs])
		AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
]], [[
@autoreleasepool {
	for (id<MTLDevice> device in MTLCopyAllDevices()) {
		NSString *name = device.name;
		uint64_t registry_id = device.registryID;
		BOOL unified = device.hasUnifiedMemory;
		uint64_t working_set = device.recommendedMaxWorkingSetSize;
		(void) name;
		(void) registry_id;
		(void) unified;
		(void) working_set;
	}
}
]])], [have_metal=yes])
		AC_MSG_RESULT([$have_metal])
		AC_LANG_POP([Objective C])
		OBJCFLAGS=$slurm_save_OBJCFLAGS
		LIBS=$slurm_save_LIBS
	])
	AS_IF([test "x$have_metal" = xyes], [
		AC_DEFINE([HAVE_METAL], [1], [Define if Metal GPU discovery is available])
	], [
		METAL_OBJCFLAGS=""
		METAL_LIBS=""
		AS_IF([test "x$enable_metal" = xyes],
			[AC_MSG_ERROR([Metal needs a macOS SDK, Objective-C ARC, and a deployment target with the macOS 10.15 device APIs])])
	])
	AC_SUBST([METAL_OBJCFLAGS])
	AC_SUBST([METAL_LIBS])
	AM_CONDITIONAL([BUILD_METAL], [test "x$have_metal" = xyes])
])
