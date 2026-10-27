/* Bulwark engine - runs one function of a progs file with only the core
 * builtins. Used by the VM tests. ISC licence, see LICENSE. */
#include "../src/qcvm/qcvm.h"
#include "../src/common/cvar.h"
#include "../src/common/cmd.h"
#include "../src/common/fs.h"

#include <stdio.h>
#include <stdlib.h>

/* model 1 is the skeletal test model, found from the source tree's root */
static const char *test_modelname(qcvm_t *vm, int index)
{
	return index == 1 ? "tests/data/skeltest.iqm" : NULL;
}

int main(int argc, char **argv)
{
	static const qc_builtin_t *const tables[] = {qc_builtins_core, qc_builtins_extra, NULL};
	const char *func = argc > 2 ? argv[2] : "main";
	size_t len;
	FILE *f;
	char *data;
	qcvm_t *vm;
	bool ok;

	if (argc < 2) {
		fprintf(stderr, "usage: qcrun <progs.dat> [function]\n");
		return 2;
	}
	Cmd_Init();
	Cvar_Init();
	FS_Init(".");
	FS_AddGameDir(".");
	f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "can't open %s\n", argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(len);
	if (fread(data, 1, len, f) != len) {
		fprintf(stderr, "can't read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	vm = QC_Load(argv[1], data, len, QCVM_TEST, tables, 256, 1 << 20);
	free(data);
	if (!vm)
		return 2;
	vm->modelname = test_modelname;
	if (!QC_FindFunction(vm, func)) {
		fprintf(stderr, "no function %s\n", func);
		return 2;
	}
	ok = QC_Execute(vm, QC_FindFunction(vm, func));
	QC_CollectGarbage(vm, true);
	QC_Free(vm);
	return ok ? 0 : 1;
}
