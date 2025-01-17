#include "llvm_propeller_profile_writer.h"

#include <memory>
#include <sstream>
#include <string>

#include "llvm_propeller_bbsections.h"
#include "llvm_propeller_cfg.h"
#include "llvm_propeller_formatting.h"
#include "llvm_propeller_options.pb.h"
#include "llvm_propeller_options_builder.h"
#include "llvm_propeller_whole_program_info.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/abseil/absl/flags/flag.h"
#include "third_party/abseil/absl/strings/str_cat.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#define FLAGS_test_tmpdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

#define FLAGS_test_srcdir std::string(testing::UnitTest::GetInstance()->original_working_dir())

namespace {

using ::devtools_crosstool_autofdo::PropellerOptions;
using ::devtools_crosstool_autofdo::PropellerOptionsBuilder;
using ::devtools_crosstool_autofdo::PropellerProfWriter;
using ::devtools_crosstool_autofdo::PropellerWholeProgramInfo;

TEST(LlvmPropellerProfileWriterTest, FindBinaryBuildId) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_barebone_nopie_buildid.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder().SetBinaryName(binary)));
  ASSERT_TRUE(pwi);
  EXPECT_FALSE(pwi->binary_is_pie());

  const std::string build_id_file =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_barebone_nopie_buildid.build-id");
  auto mb = llvm::MemoryBuffer::getFile(build_id_file);
  EXPECT_FALSE(mb.getError());
  std::unique_ptr<llvm::MemoryBuffer> &mbp = *mb;
  EXPECT_GE(mbp->getBufferSize(), 16);
  EXPECT_EQ(0,
            pwi->binary_build_id().compare(0, 16, mbp->getBufferStart(), 16));
}

TEST(LlvmPropellerProfileWriterTest, PieAndNoBuildId) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_barebone_pie_nobuildid.bin");
  std::unique_ptr<PropellerWholeProgramInfo> pwi =
      PropellerWholeProgramInfo::Create(
          PropellerOptions(PropellerOptionsBuilder().SetBinaryName(binary)));
  ASSERT_TRUE(pwi);
  EXPECT_TRUE(pwi->binary_is_pie());
  EXPECT_TRUE(pwi->binary_build_id().empty());
}


TEST(LlvmPropellerProfileWriterTest, ParsePerf0_relative_path) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  // Test that specifying --mmap_name with relative name works.
  auto writer_ptr = PropellerProfWriter::Create(PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("any_relative_path/propeller_sample.bin")));
  // When relative file path is passed to "--binary", we use the name portion of
  // the path (like `basename <filename>`) to match mmap entries. Since
  // propeller_sample.perfdata contains mmaps with file name
  // "/usr/local/google/home/shenhan/copt/llvm-propeller-2/plo/propeller_sample.bin",
  // writer_ptr must not be null.
  ASSERT_NE(nullptr, writer_ptr);
}

TEST(LlvmPropellerProfileWriterTest, ParsePerf0_absolute_path) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  auto writer_ptr = PropellerProfWriter::Create(PropellerOptions(
      PropellerOptionsBuilder()
          .SetBinaryName(binary)
          .AddPerfNames(perfdata)
          .SetClusterOutName("dummy.out")
          .SetProfiledBinaryName("/any_absolute_path/propeller_sample.bin")));
  // When absolute file path is passed to "--binary", we use absolute path to
  // match mmap entries. Since propeller_sample.perfdata only contains mmap with
  // file name
  // "/usr/local/google/home/shenhan/copt/llvm-propeller-2/plo/propeller_sample.bin",
  // this is expected to fail.
  ASSERT_EQ(nullptr, writer_ptr);
}

TEST(LlvmPropellerProfileWriterTest, ParsePerf1) {
  const std::string binary =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.bin");
  const std::string perfdata =
      absl::StrCat(FLAGS_test_srcdir,
                   "/testdata/"
                   "propeller_sample.perfdata");
  auto writer_ptr = PropellerProfWriter::Create(
      PropellerOptions(PropellerOptionsBuilder()
                           .SetBinaryName(binary)
                           .AddPerfNames(perfdata)
                           .SetClusterOutName("dummy.out")
                           .SetProfiledBinaryName("propeller_sample.bin")
                           .SetKeepFrontendIntermediateData(true)));
  ASSERT_NE(nullptr, writer_ptr);
  const PropellerWholeProgramInfo *pwi =
      static_cast<const PropellerWholeProgramInfo *>(
          writer_ptr->whole_program_info());
  // We started 3 instances, so mmap must be >= 3.
  EXPECT_GE(pwi->binary_mmaps().size(), 3);

  // All instances of the same pie binary cannot be loaded into same place.
  bool all_equals = true;
  for (auto i = pwi->binary_mmaps().begin(),
            e = std::prev(pwi->binary_mmaps().end());
       i != e; ++i)
    all_equals &= (i->second == std::next(i)->second);
  EXPECT_FALSE(all_equals);
}
}  // namespace
