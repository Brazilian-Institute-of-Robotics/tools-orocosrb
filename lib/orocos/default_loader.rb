module Orocos
    class DefaultLoader < OroGen::Loaders::Aggregate
        # @return [Boolean] whether the types that get registered on {registry}
        #   should be exported as Ruby constants
        attr_predicate :export_types?

        # The namespace in which the types should be exported if
        # {export_types?} returns true. It defaults to Types
        #
        # @return [Module]
        attr_reader :type_export_namespace

        def initialize
            @type_export_namespace = ::Types
            # We need recursive access lock
            @load_access_lock = Monitor.new
            super
            self.export_types = true
        end

        def export_types=(flag)
            if !export_types? && flag
                registry.export_to_ruby(type_export_namespace) do |type, exported_type|
                    if type.name =~ /orogen_typekits/ # just ignore those
                    elsif type <= Typelib::NumericType # using numeric is transparent in Typelib/Ruby
                    elsif type.contains_opaques? # register the intermediate instead
                        intermediate_type_for(type)
                    elsif m_type?(type) # just ignore, they are registered as the opaque
                    else exported_type
                    end
                end
                @export_types = true
            elsif export_types? && !flag
                type_export_namespace.disable_registry_export
                @export_types = false
            end
        end

        def clear
            if export_types? && registry
                type_export_namespace.reset_registry_export
            end
            super
            OroGen::Loaders::RTT.setup_loader(self)
        end

        def register_project_model(project)
            super
            project.self_tasks.each_value do |task|
                task.each_extension do |ext|
                    Orocos.load_extension_runtime_library(ext.name)
                end
            end
        end

        def task_model_from_name(name)
            @load_access_lock.synchronize do
                super
            end
        end
    end
end
    

