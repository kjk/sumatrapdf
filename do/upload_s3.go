package main

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

const (
	s3PreRelDir  = "sumatrapdf/prerel/"
	s3RelDir     = "sumatrapdf/rel/"
	maxS3Results = 1000
)

// we should only sign and upload to s3 if this is my repo
// and a push event
func shouldSignOrUpload() bool {
	// https://help.github.com/en/actions/automating-your-workflow-with-github-actions/using-environment-variables

	repo := os.Getenv("GITHUB_REPOSITORY")
	if repo != "sumatrapdfreader/sumatrapdf" {
		return false
	}
	event := os.Getenv("GITHUB_EVENT_NAME")
	// other event is "pull_request"
	return event == "push"
}

// sumatrapdf/sumatralatest.js
/*
var sumLatestVer = 10175;
var sumBuiltOn = "2015-07-23";
var sumLatestName = "SumatraPDF-prerelease-10175.exe";
var sumLatestExe = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-10175.exe";
var sumLatestPdb = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-10175.pdb.zip";
var sumLatestInstaller = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-10175-install.exe";
*/
func createSumatraLatestJs() string {
	currDate := time.Now().Format("2006-01-02")
	v := svnPreReleaseVer
	return fmt.Sprintf(`
		var sumLatestVer = %s;
		var sumBuiltOn = "%s";
		var sumLatestName = "SumatraPDF-prerelease-%s.exe";

		var sumLatestExe = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s.exe";
		var sumLatestPdb = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s.pdb.zip";
		var sumLatestInstaller = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s-install.exe";

		var sumLatestExe64 = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s-64.exe";
		var sumLatestPdb64 = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s-64.pdb.zip";
		var sumLatestInstaller64 = "https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-%s-64-install.exe";
`, v, currDate, v, v, v, v, v, v, v)
}

// FilesForVer describes pre-release files in s3 for a given version
type FilesForVer struct {
	Version    int      // pre-release version as int
	VersionStr string   // pre-release version as string
	Names      []string // relative to sumatrapdf/prerel/
	Paths      []string // full key path in S3
}

/*
Recognize the following files:
SumatraPDF-prerelease-10169-install.exe
SumatraPDF-prerelease-10169.exe
SumatraPDF-prerelease-10169.pdb.lzsa
SumatraPDF-prerelease-10169.pdb.zip
SumatraPDF-prerelease-10169-install-64.exe
SumatraPDF-prerelease-10169-64.exe
SumatraPDF-prerelease-10169.pdb-64.lzsa
SumatraPDF-prerelease-10169.pdb-64.zip
manifest-10169.txt
*/

var (
	preRelNameRegexps []*regexp.Regexp
	regexps           = []string{
		`SumatraPDF-prerelease-(\d+)-install-64.exe`,
		`SumatraPDF-prerelease-(\d+)-64.exe`,
		`SumatraPDF-prerelease-(\d+).pdb-64.lzsa`,
		`SumatraPDF-prerelease-(\d+).pdb-64.zip`,

		`SumatraPDF-prerelease-(\d+)-install.exe`,
		`SumatraPDF-prerelease-(\d+).exe`,
		`SumatraPDF-prerelease-(\d+).pdb.lzsa`,
		`SumatraPDF-prerelease-(\d+).pdb.zip`,

		`manifest-(\d+).txt`,
	}
)

func compilePreRelNameRegexpsMust() {
	fatalIf(preRelNameRegexps != nil, "preRelNameRegexps != nil")
	for _, s := range regexps {
		r := regexp.MustCompile(s)
		preRelNameRegexps = append(preRelNameRegexps, r)
	}
}

func preRelFileVer(name string) string {
	for _, r := range preRelNameRegexps {
		res := r.FindStringSubmatch(name)
		if len(res) == 2 {
			return res[1]
		}
	}
	return ""
}

func addToFilesForVer(path, name, verStr string, files []*FilesForVer) []*FilesForVer {
	ver, err := strconv.Atoi(verStr)
	fatalIfErr(err)
	for _, fi := range files {
		if fi.Version == ver {
			fi.Names = append(fi.Names, name)
			fi.Paths = append(fi.Paths, path)
			return files
		}
	}

	fi := FilesForVer{
		Version:    ver,
		VersionStr: verStr,
		Names:      []string{name},
		Paths:      []string{path},
	}
	return append(files, &fi)
}

// ByVerFilesForVer sorts by version
type ByVerFilesForVer []*FilesForVer

func (s ByVerFilesForVer) Len() int {
	return len(s)
}
func (s ByVerFilesForVer) Swap(i, j int) {
	s[i], s[j] = s[j], s[i]
}
func (s ByVerFilesForVer) Less(i, j int) bool {
	return s[i].Version > s[j].Version
}

// list is sorted by Version, biggest first, to make it easy to delete oldest
func s3ListPreReleaseFilesMust(c *S3Client, dbg bool) []*FilesForVer {
	fatalIf(preRelNameRegexps == nil, "preRelNameRegexps == nil")
	var res []*FilesForVer
	bucket := c.GetBucket()
	resp, err := bucket.List(s3PreRelDir, "", "", maxS3Results)
	fatalIfErr(err)
	fatalIf(resp.IsTruncated, "truncated response! implement reading all the files\n")
	if dbg {
		logf("%d files\n", len(resp.Contents))
	}
	var unrecognizedFiles []string
	for _, key := range resp.Contents {
		path := key.Key
		name := path[len(s3PreRelDir):]
		verStr := preRelFileVer(name)
		if dbg {
			logf("path: '%s', name: '%s', ver: '%s', \n", path, name, verStr)
		}
		if verStr == "" {
			unrecognizedFiles = append(unrecognizedFiles, path)
		} else {
			res = addToFilesForVer(path, name, verStr, res)
		}
	}
	sort.Sort(ByVerFilesForVer(res))
	for _, s := range unrecognizedFiles {
		logf("Unrecognized pre-relase file in s3: '%s'\n", s)
	}

	if true || dbg {
		for _, fi := range res {
			logf("Ver: %s (%d)\n", fi.VersionStr, fi.Version)
			logf("  names: %s\n", fi.Names)
			logf("  paths: %s\n", fi.Paths)
		}
	}
	return res
}

// we shouldn't re-upload files. We upload manifest-${ver}.txt last, so we
// consider a pre-release build already present in s3 if manifest file exists
func verifyPreReleaseNotInS3Must(c *S3Client, ver string) {
	if !flgUpload {
		return
	}
	s3Path := s3PreRelDir + fmt.Sprintf("SumatraPDF-prerelease-%s-manifest.txt", ver)
	fatalIf(c.Exists(s3Path), "build %s already exists in s3 because '%s' exists\n", ver, s3Path)
}

func verifyReleaseNotInS3Must(c *S3Client, ver string) {
	if !flgUpload {
		return
	}
	s3Path := s3RelDir + fmt.Sprintf("SumatraPDF-%s-manifest.txt", ver)
	fatalIf(c.Exists(s3Path), "build '%s' already exists in s3 because '%s' existst\n", ver, s3Path)
}

func s3DeleteOldestPreRel(c *S3Client) {
	if !flgUpload {
		return
	}
	maxToRetain := 10
	files := s3ListPreReleaseFilesMust(c, false)
	if len(files) < maxToRetain {
		return
	}
	toDelete := files[maxToRetain:]
	for _, fi := range toDelete {
		for _, s3Path := range fi.Paths {
			// don't delete manifest files
			if strings.Contains(s3Path, "manifest-") {
				continue
			}
			err := c.Delete(s3Path)
			if err != nil {
				// it's ok if fails, we'll try again next time
				logf("Failed to delete '%s' in s3\n", s3Path)
			}
		}
	}
}

func dumpEnv() {
	env := os.Environ()
	logf("\nEnv:\n")
	for _, s := range env {
		logf("env: %s\n", s)
	}
	logf("\n")
}

func isMaster() bool {
	ref := os.Getenv("GITHUB_REF")
	return ref == "refs/heads/master"
}

func shouldSkipUpload() bool {
	if !flgUpload {
		logf("Skipping pre-release upload to s3 because -upload flag not given\n")
		return true
	}

	if !isMaster() {
		logf("Skipping pre-release upload to s3 because not on master branch\n")
		logf("GITHUB_REF: '%s'\n", os.Getenv("GITHUB_REF"))
		flgUpload = false
		return true
	}

	if !shouldSignOrUpload() {
		logf("skipping upload beacuse not my repo\n")
		flgUpload = false
		return true
	}
	return false
}

// upload as:
// https://kjkpub.s3.amazonaws.com/sumatrapdf/prerel/SumatraPDF-prerelease-1027-install.exe etc.
func s3UploadPreReleaseMust(ver string) {
	if shouldSkipUpload() {
		return
	}

	c := &S3Client{
		Access: os.Getenv("AWS_ACCESS"),
		Secret: os.Getenv("AWS_SECRET"),
		Bucket: "kjkpub",
	}
	//dumpEnv()
	c.VerifyHasSecrets()

	timeStart := time.Now()
	//s3DeleteOldestPreRel()

	verifyPreReleaseNotInS3Must(c, svnPreReleaseVer)

	prefix := fmt.Sprintf("SumatraPDF-prerelease-%s", ver)
	manifestRemotePath := s3PreRelDir + prefix + "-manifest.txt"
	files := []string{
		"SumatraPDF.exe", fmt.Sprintf("%s.exe", prefix),
		"SumatraPDF-dll.exe", fmt.Sprintf("%s-install.exe", prefix),
		"SumatraPDF.pdb.zip", fmt.Sprintf("%s.pdb.zip", prefix),
		"SumatraPDF.pdb.lzsa", fmt.Sprintf("%s.pdb.lzsa", prefix),
	}
	err := c.UploadFiles(s3PreRelDir, filepath.Join("out", "rel32"), files)
	fatalIfErr(err)

	prefix = fmt.Sprintf("SumatraPDF-prerelease-%s-64", ver)
	files = []string{
		"SumatraPDF.exe", fmt.Sprintf("%s.exe", prefix),
		"SumatraPDF-dll.exe", fmt.Sprintf("%s-install.exe", prefix),
		"SumatraPDF.pdb.zip", fmt.Sprintf("%s.pdb.zip", prefix),
		"SumatraPDF.pdb.lzsa", fmt.Sprintf("%s.pdb.lzsa", prefix),
	}
	err = c.UploadFiles(s3PreRelDir, filepath.Join("out", "rel64"), files)
	fatalIfErr(err)

	manifestLocalPath := filepath.Join(artifactsDir, "manifest.txt")
	err = c.UploadFileReader(manifestRemotePath, manifestLocalPath, true)
	fatalIfErr(err)

	s3UploadDailyInfo(c, ver)

	logf("Uploaded the build to s3 in %s\n", time.Since(timeStart))
	//s3SetAsPreRelease(ver)
}

func s3UploadDailyInfo(c *S3Client, ver string) {
	s := createSumatraLatestJs()
	err := c.UploadString("sumatrapdf/sumadaily.js", s, true)
	fatalIfErr(err)

	//sumatrapdf/sumpdf-prerelease-latest.txt
	err = c.UploadString("sumatrapdf/sumpdf-daily-latest.txt", ver, true)
	fatalIfErr(err)

	//sumatrapdf/sumpdf-prerelease-update.txt
	//don't set a Stable version for pre-release builds
	s = fmt.Sprintf("[SumatraPDF]\nLatest %s\n", ver)
	err = c.UploadString("sumatrapdf/sumpdf-daily-update.txt", s, true)
	fatalIfErr(err)
}

func s3SetAsPreRelease(c *S3Client, ver string) {
	s := createSumatraLatestJs()
	err := c.UploadString("sumatrapdf/sumatralatest.js", s, true)
	fatalIfErr(err)

	//sumatrapdf/sumpdf-prerelease-latest.txt
	err = c.UploadString("sumatrapdf/sumpdf-prerelease-latest.txt", ver, true)
	fatalIfErr(err)

	//sumatrapdf/sumpdf-prerelease-update.txt
	//don't set a Stable version for pre-release builds
	s = fmt.Sprintf("[SumatraPDF]\nLatest %s\n", ver)
	err = c.UploadString("sumatrapdf/sumpdf-prerelease-update.txt", s, true)
	fatalIfErr(err)
}
