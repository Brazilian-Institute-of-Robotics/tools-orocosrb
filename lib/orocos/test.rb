require 'fileutils'
require 'typelib'
require 'orogen'

module Orocos
    module Test
        attr_reader :processes

        def setup
            @processes = Array.new
            super if defined? super
        end

        def teardown
            processes.each { |p| p.kill }
            processes.clear
            super if defined? super
        end

        # Generates, builds and installs the orogen component defined by the
        # orogen description file +src+. The compiled package is installed in
        # +prefix+
        def self.generate_and_build(src, work_basedir)
            src_dir  = File.dirname(src)
            src_name = File.basename(src_dir)

            FileUtils.mkdir_p work_basedir
            work_dir = File.join(work_basedir, src_name)
            FileUtils.rm_rf work_dir
            FileUtils.cp_r  src_dir, work_dir

            prefix   = File.join(work_basedir, "prefix")
            ruby_bin   = RbConfig::CONFIG['RUBY_INSTALL_NAME']
            orogen_bin = File.expand_path('../bin/orogen', Orocos::Generation.base_dir)
            Dir.chdir(work_dir) do
                if !system(ruby_bin, orogen_bin, '--corba', File.basename(src))
                    raise "failed to build"
                end

                if !File.directory? 'build'
                    FileUtils.mkdir 'build'
                end
                Dir.chdir 'build' do
                    if !system 'cmake', "-DCMAKE_INSTALL_PREFIX=#{prefix}", ".."
                        raise "failed to configure"
                    elsif !system "make", "install"
                        raise "failed to install"
                    end
                end
            end
            ENV['PKG_CONFIG_PATH'] += ":#{prefix}/lib/pkgconfig"
        end

        def spawn_and_get(component, task = component)
            begin
                process = Orocos::Process.new component
                process.spawn
                process.wait_running(0.5)
            rescue Exception
                process.kill if process
                raise
            end

            processes << process
            Orocos::TaskContext.get "#{component}.#{task}"
        end
    end

    module Spec
        def setup
            ENV['PKG_CONFIG_PATH'] = "#{File.join(WORK_DIR, "prefix", 'lib', 'pkgconfig')}"
            Orocos.initialize

            @old_timeout = Orocos::CORBA.connect_timeout
            Orocos::CORBA.connect_timeout = 50
            @old_pkg_config = ENV['PKG_CONFIG_PATH'].dup
            super
        end
        def teardown
            super
            Orocos::CORBA.connect_timeout = @old_timeout if @old_timeout
            Orocos.instance_variable_set :@registry, Typelib::Registry.new
            Orocos::CORBA.instance_variable_set :@loaded_toolkits, []
            ENV['PKG_CONFIG_PATH'] = @old_pkg_config
        end
    end
end

