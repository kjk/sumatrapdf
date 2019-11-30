package main

import (
	"archive/zip"
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/kjk/u"
)

var (
	pdbFiles = []string{"libmupdf.pdb", "Installer.pdb",
		"SumatraPDF-dll.pdb", "SumatraPDF.pdb"}
)

var (
	svnPreReleaseVer string
	gitSha1          string
	sumatraVersion   string
)

func isNum(s string) bool {
	_, err := strconv.Atoi(s)
	return err == nil
}

// Version must be in format x.y.z
func verifyCorrectVersionMust(ver string) {
	parts := strings.Split(ver, ".")
	u.PanicIf(len(parts) == 0 || len(parts) > 3, "%s is not a valid version number", ver)
	for _, part := range parts {
		u.PanicIf(!isNum(part), "%s is not a valid version number", ver)
	}
}

func extractSumatraVersionMust() string {
	path := filepath.Join("src", "Version.h")
	d := u.ReadFileMust(path)
	lines := toTrimmedLines(d)
	s := "#define CURR_VERSION "
	for _, l := range lines {
		if strings.HasPrefix(l, s) {
			ver := l[len(s):]
			verifyCorrectVersionMust(ver)
			return ver
		}
	}
	panic(fmt.Sprintf("couldn't extract CURR_VERSION from %s\n", path))
}

func detectVersions() {
	ver := getGitLinearVersionMust()
	svnPreReleaseVer = strconv.Itoa(ver)
	gitSha1 = getGitSha1Must()
	sumatraVersion = extractSumatraVersionMust()
	logf("svnPreReleaseVer: '%s'\n", svnPreReleaseVer)
	logf("gitSha1: '%s'\n", gitSha1)
	logf("sumatraVersion: '%s'\n", sumatraVersion)
}

func clean() {
	os.RemoveAll("out")
}

func runTestUtilMust(dir string) {
	cmd := exec.Command(`.\test_util.exe`)
	cmd.Dir = dir
	u.RunCmdLoggedMust(cmd)
}

func buildLzsa() {
	// early exit if missing
	detectSigntoolPath()

	defer makePrintDuration("buildLzsa")()
	clean()

	msbuildPath := detectMsbuildPath()
	runExeLoggedMust(msbuildPath, `vs2019\SumatraPDF.sln`, `/t:MakeLZSA`, `/p:Configuration=Release;Platform=Win32`, `/m`)

	path := filepath.Join("out", "rel32", "MakeLZSA.exe")
	signMust(path)
}

// smoke build is meant to be run locally to check that we can build everything
// it does full installer build of 64-bit release build
// We don't build other variants for speed. It takes about 5 mins locally
func smokeBuild() {
	detectSigntoolPath()
	defer makePrintDuration("smoke build")()
	clean()

	lzsa := absPathMust(filepath.Join("bin", "MakeLZSA.exe"))
	u.PanicIf(!u.FileExists(lzsa), "file '%s' doesn't exist", lzsa)

	msbuildPath := detectMsbuildPath()
	runExeLoggedMust(msbuildPath, `vs2019\SumatraPDF.sln`, `/t:SumatraPDF-dll;test_util`, `/p:Configuration=Release;Platform=x64`, `/m`)
	runTestUtilMust(filepath.Join("out", "rel64"))

	{
		cmd := exec.Command(lzsa, "SumatraPDF.pdb.lzsa", "libmupdf.pdb:libmupdf.pdb", "SumatraPDF.pdb:SumatraPDF.pdb", "SumatraPDF-dll.pdb:SumatraPDF-dll.pdb")
		cmd.Dir = filepath.Join("out", "rel64")
		u.RunCmdLoggedMust(cmd)
	}
}

func buildConfigPath() string {
	return filepath.Join("src", "utils", "BuildConfig.h")
}

// writes src/utils/BuildConfig.h to over-ride some of build settings
func setBuildConfig(sha1, preRelVer string) {
	fatalIf(sha1 == "", "sha1 must be set")
	s := fmt.Sprintf("#define GIT_COMMIT_ID %s\n", sha1)
	if preRelVer != "" {
		s += fmt.Sprintf("#define SVN_PRE_RELEASE_VER %s\n", preRelVer)
	}
	err := ioutil.WriteFile(buildConfigPath(), []byte(s), 644)
	fatalIfErr(err)
}

func revertBuildConfig() {
	runExeMust("git", "checkout", buildConfigPath())
}

func addZipFileMust(w *zip.Writer, path string) {
	fi, err := os.Stat(path)
	fatalIfErr(err)
	fih, err := zip.FileInfoHeader(fi)
	fatalIfErr(err)
	fih.Name = filepath.Base(path)
	fih.Method = zip.Deflate
	d, err := ioutil.ReadFile(path)
	fatalIfErr(err)
	fw, err := w.CreateHeader(fih)
	fatalIfErr(err)
	_, err = fw.Write(d)
	fatalIfErr(err)
	// fw is just a io.Writer so we can't Close() it. It's not necessary as
	// it's implicitly closed by the next Create(), CreateHeader()
	// or Close() call on zip.Writer
}

func createExeZipWithPigz(dir string) {
	srcFile := "SumatraPDF.exe"
	srcPath := filepath.Join(dir, srcFile)
	fatalIf(!u.FileExists(srcPath), "file '%s' doesn't exist\n", srcPath)

	// this is the file that pigz.exe will create
	dstFileTmp := "SumatraPDF.exe.zip"
	dstPathTmp := filepath.Join(dir, dstFileTmp)
	removeFileMust(dstPathTmp)

	// this is the file we want at the end
	dstFile := "SumatraPDF.zip"
	dstPath := filepath.Join(dir, dstFile)
	removeFileMust(dstPath)

	wd, err := os.Getwd()
	fatalIfErr(err)
	pigzExePath := filepath.Join(wd, "bin", "pigz.exe")
	fatalIf(!u.FileExists(pigzExePath), "file '%s' doesn't exist\n", pigzExePath)
	cmd := exec.Command(pigzExePath, "-11", "--keep", "--zip", srcFile)
	// in pigz we don't control the name of the file created inside so
	// so when we run pigz the current directory is the same as
	// the directory with the file we're compressing
	cmd.Dir = dir
	u.RunCmdMust(cmd)

	fatalIf(!u.FileExists(dstPathTmp), "file '%s' doesn't exist\n", dstPathTmp)
	err = os.Rename(dstPathTmp, dstPath)
	fatalIfErr(err)
}

// createExeZipWithGoMust() is faster, createExeZipWithPigz() generates slightly
// smaller files
func createExeZipMust(dir string) {
	//createExeZipWithGoMust(dir)
	createExeZipWithPigz(dir)
}

func createPdbZipMust(dir string) {
	path := filepath.Join(dir, "SumatraPDF.pdb.zip")
	f, err := os.Create(path)
	fatalIfErr(err)
	defer f.Close()
	w := zip.NewWriter(f)

	for _, file := range pdbFiles {
		addZipFileMust(w, filepath.Join(dir, file))
	}

	err = w.Close()
	fatalIfErr(err)
}

func createPdbLzsaMust(dir string) {
	args := []string{"SumatraPDF.pdb.lzsa"}
	args = append(args, pdbFiles...)
	curDir, err := os.Getwd()
	fatalIfErr(err)
	makeLzsaPath := filepath.Join(curDir, "bin", "MakeLZSA.exe")
	cmd := exec.Command(makeLzsaPath, args...)
	cmd.Dir = dir
	u.RunCmdLoggedMust(cmd)
}

// manifest is build for pre-release builds and contains information about file sizes
func createManifestMust() {
	var lines []string
	files := []string{
		"SumatraPDF.exe",
		"SumatraPDF-dll.exe",
		"libmupdf.dll",
		"PdfFilter.dll",
		"PdfPreview.dll",
		"SumatraPDF.pdb.zip",
		"SumatraPDF.pdb.lzsa",
	}
	dirs := []string{filepath.Join("out", "rel32"), filepath.Join("out", "rel64")}
	for _, dir := range dirs {
		for _, file := range files {
			path := filepath.Join(dir, file)
			size := fileSizeMust(path)
			line := fmt.Sprintf("%s: %d", path, size)
			lines = append(lines, line)
		}
	}
	s := strings.Join(lines, "\n")
	path := filepath.Join(artifactsDir, "manifest.txt")
	u.WriteFileMust(path, []byte(s))
}

func buildPreRelease() {
	// early exit if missing
	detectSigntoolPath()

	s := fmt.Sprintf("buidling pre-release version %s", svnPreReleaseVer)
	defer makePrintDuration(s)()
	clean()

	verifyGitCleanMust()
	verifyOnMasterBranchMust()

	verifyTranslationsMust()

	setBuildConfig(gitSha1, svnPreReleaseVer)
	defer revertBuildConfig()

	msbuildPath := detectMsbuildPath()
	slnPath := filepath.Join("vs2019", "SumatraPDF.sln")

	// we want to sign files inside the installer, so we have to
	runExeLoggedMust(msbuildPath, slnPath, `/t:SumatraPDF;SumatraPDF-dll;PdfFilter;PdfPreview;test_util`, `/p:Configuration=Release;Platform=Win32`, `/m`)

	dir := filepath.Join("out", "rel32")
	runTestUtilMust(dir)
	signFilesMust(dir)

	runExeLoggedMust(msbuildPath, slnPath, "/t:SumatraPDF;SumatraPDF-dll;PdfFilter;PdfPreview;test_util", "/p:Configuration=Release;Platform=x64", "/m")

	dir = filepath.Join("out", "rel64")
	runTestUtilMust(dir)
	signFilesMust(dir)

	createPdbZipMust(filepath.Join("out", "rel32"))
	createPdbZipMust(filepath.Join("out", "rel64"))

	createPdbLzsaMust(filepath.Join("out", "rel32"))
	createPdbLzsaMust(filepath.Join("out", "rel64"))

	copyArtifacts()
	createManifestMust()

	s3UploadPreReleaseMust(svnPreReleaseVer)
}

const (
	artifactsDir = "artifacts"
)

var (
	artifactFiles = []string{
		"SumatraPDF.exe",
		"SumatraPDF-dll.exe",
		"SumatraPDF.pdb.lzsa",
		"SumatraPDF.pdb.zip",
	}
)

// TODO: add version number to file names (like "Installer-3.2.0-pre3333")
// TODO: make "SumatraPDF.exe.zip" from "SumatraPDF.exe", for smaller downloads
func copyArtifactsFiles(dstDir, srcDir string) {
	u.CreateDirIfNotExistsMust(dstDir)
	for _, f := range artifactFiles {
		src := filepath.Join(srcDir, f)
		dst := filepath.Join(dstDir, f)
		u.CopyFileMust(dst, src)
	}
}

// This is for the benefit of GitHub Actions: copy files to artifacts directory
func copyArtifacts() {
	copyArtifactsFiles(filepath.Join(artifactsDir, "32"), filepath.Join("out", "rel32"))
	copyArtifactsFiles(filepath.Join(artifactsDir, "64"), filepath.Join("out", "rel64"))
}

func signFilesMust(dir string) {
	signMust(filepath.Join(dir, "SumatraPDF.exe"))
	signMust(filepath.Join(dir, "libmupdf.dll"))
	signMust(filepath.Join(dir, "PdfFilter.dll"))
	signMust(filepath.Join(dir, "PdfPreview.dll"))
	signMust(filepath.Join(dir, "SumatraPDF-dll.exe"))
}

func buildRelease() {
	// TODO: implement me
}
